"""A Chamber is one physical module's local simulation space.

Holds the grid, the queen (if this is the founding chamber), brood, and
workers. Chambers are driven by the Coordinator's per-tick loop. Only
the chamber knows about per-cell geometry; higher layers treat it as a
black box with a population of entities.

A Chamber also tracks its neighbour on each of its four faces via the
`entries` dict (face -> neighbour module_id or None). When a worker
steps onto an entry cell on an active face the chamber flags it for
handoff and the Coordinator moves the ant to the opposite face of the
neighbour on the same tick.
"""

import config as C
from sim.queen import Queen
from sim.ant import Ant, OUTBOUND
from sim import brood as brood_mod
from sim.pheromones import PheromoneMap


# Kinds of chamber. Phase 2 first cut only uses 'chamber'; 'outworld'
# and 'tunnel' will land with foraging.
KIND_CHAMBER  = 'chamber'
KIND_OUTWORLD = 'outworld'
KIND_TUNNEL   = 'tunnel'


class Chamber:
    def __init__(self, module_id, colony, has_queen=True, kind=KIND_CHAMBER):
        self.module_id = module_id
        self.colony    = colony
        self.kind      = kind
        self.width     = C.GRID_WIDTH
        self.height    = C.GRID_HEIGHT

        self.queen = None
        if has_queen:
            qx, qy = C.QUEEN_SPAWN
            self.queen = Queen(qx, qy)

        self.brood   = []       # list[Brood]
        self.workers = []       # list[Ant]

        # face -> neighbour module_id (None = no neighbour attached).
        self.entries = {face: None for face in C.ENTRY_POINTS}

        # face -> neighbour Chamber reference (None = no neighbour).
        # Mirrors self.entries, but holds the actual Chamber object so
        # deposit/sense helpers can cross the corridor without going
        # through the coordinator every call. Populated by the
        # Coordinator when a new module is attached.
        self.neighbors = {face: None for face in C.ENTRY_POINTS}

        # Which face points home-ward (toward the founding chamber).
        # Set to None for the founding chamber itself; otherwise the
        # Coordinator runs a BFS from founding after every module
        # attachment and fills this in. Returning ants walk toward
        # the entry on this face, not the nearest entry, so multi-hop
        # topologies don't cause oscillation between intermediate
        # chambers.
        self.home_face = None

        # Physical food piles — {(x, y): amount}. Populated via the F
        # debug hotkey or (Phase 3) by outworld food-source spawning.
        self.food_cells = {}

        # Two pheromone layers for the trail-based foraging loop.
        self.pheromones = PheromoneMap(self.width, self.height)

    # ---- queries ----

    def in_bounds(self, x, y):
        return 0 <= x < self.width and 0 <= y < self.height

    def count_brood(self):
        counts = {'egg': 0, 'larva': 0, 'pupa': 0}
        for b in self.brood:
            if not b.alive:
                continue
            if b.stage in counts:
                counts[b.stage] += 1
        return counts

    # ---- food piles ----

    def add_food(self, x, y, amount):
        """Spawn (or grow) a food pile on a grid cell."""
        if not self.in_bounds(x, y):
            return
        self.food_cells[(x, y)] = self.food_cells.get((x, y), 0.0) + amount

    def take_food(self, x, y, amount):
        """Remove up to `amount` food from the pile at (x, y). Returns
        the amount actually taken."""
        pile = self.food_cells.get((x, y), 0.0)
        if pile <= 0.0:
            return 0.0
        taken = min(amount, pile)
        remaining = pile - taken
        if remaining <= 0.0:
            del self.food_cells[(x, y)]
        else:
            self.food_cells[(x, y)] = remaining
        return taken

    # ---- pheromone deposits (corridor-aware) ----

    def deposit_home(self, x, y, amount, ant_dx, ant_dy):
        """Deposit home_scent at (x, y) and, if (x, y) is an active
        entry cell, mirror the deposit to the neighbour chamber's
        opposite entry cell. Mirroring keeps the two sides of the
        corridor in lockstep so cross-boundary sensing has signal
        at the entry cell — without this, ants ray-casting toward
        a corridor see their own side's deposits and nothing from
        the neighbour, which creates a dead zone right at the
        crossing point.
        """
        self.pheromones.deposit_home(x, y, amount, ant_dx, ant_dy)
        face = self._entry_face_at(x, y)
        if face is None:
            return
        neighbor = self.neighbors.get(face)
        if neighbor is None:
            return
        opp = C.FACE_OPPOSITE[face]
        nx, ny = C.ENTRY_POINTS[opp]
        neighbor.pheromones.deposit_home(nx, ny, amount, ant_dx, ant_dy)

    def deposit_food(self, x, y, amount, ant_dx, ant_dy):
        """Deposit food_scent locally and, if on an active entry cell,
        mirror to the neighbour's opposite entry cell. See deposit_home."""
        self.pheromones.deposit_food(x, y, amount, ant_dx, ant_dy)
        face = self._entry_face_at(x, y)
        if face is None:
            return
        neighbor = self.neighbors.get(face)
        if neighbor is None:
            return
        opp = C.FACE_OPPOSITE[face]
        nx, ny = C.ENTRY_POINTS[opp]
        neighbor.pheromones.deposit_food(nx, ny, amount, ant_dx, ant_dy)

    def _entry_face_at(self, x, y):
        """If (x, y) is the entry cell for one of this chamber's faces,
        return the face letter. Otherwise None. Used to detect when a
        deposit should be mirrored across the corridor."""
        for face, entry_pos in C.ENTRY_POINTS.items():
            if entry_pos == (x, y):
                return face
        return None

    def nearest_food_within(self, x, y, radius):
        """Return (fx, fy) of the closest non-empty food cell within
        the given Manhattan radius, or None."""
        best = None
        best_d = radius + 1
        for (fx, fy), amt in self.food_cells.items():
            if amt <= 0.0:
                continue
            d = abs(fx - x) + abs(fy - y)
            if d < best_d:
                best_d = d
                best = (fx, fy)
        return best

    # ---- per-tick ----

    def tick(self, coordinator=None):
        # Pheromones decay before any fresh deposits are made.
        self.pheromones.decay()

        # Queen
        if self.queen is not None:
            self.queen.tick(self)

        # Brood — advance stages; hatch pupae into nanitics.
        dead_brood = []
        hatched = []
        for b in self.brood:
            result = b.tick()
            if result == 'hatch':
                hatched.append(b)
            elif not b.alive:
                dead_brood.append(b)

        for b in hatched:
            self.workers.append(Ant(b.x, b.y, caste=b.caste))
            self.brood.remove(b)
        for b in dead_brood:
            self.brood.remove(b)

        # Workers
        dead_workers = []
        crossings = []     # ants that landed on an active entry this tick
        for w in self.workers:
            w.tick(self)
            if not w.alive:
                dead_workers.append(w)
                continue
            crossing = self._check_edge_crossing(w)
            if crossing is not None:
                crossings.append((w, crossing))

        for w in dead_workers:
            self.workers.remove(w)

        # Hand crossings off to the coordinator (if we have one)
        if coordinator is not None and crossings:
            for w, face in crossings:
                neighbour_id = self.entries.get(face)
                if neighbour_id is None:
                    continue
                # Suppress accidental "go home" crossings by empty
                # outbound ants. If a scout wandered onto the
                # home-face entry without cargo, that's a navigation
                # mistake — they were supposed to be scouting AWAY
                # from home, and bouncing back to the nest empty
                # just burns a round trip. Refuse the crossing; the
                # ant stays on the entry cell and their next tick's
                # _do_outbound will walk them back off it.
                if (face == self.home_face
                        and w.state == OUTBOUND
                        and w.food_carried <= 0):
                    continue
                if w in self.workers:
                    self.workers.remove(w)
                coordinator.handoff(w, self.module_id, neighbour_id, face)

    def _check_edge_crossing(self, worker):
        """Return the face whose entry cell this worker is sitting on,
        if that face is active. Otherwise None."""
        pos = (worker.x, worker.y)
        for face, entry_pos in C.ENTRY_POINTS.items():
            if pos == entry_pos and self.entries.get(face) is not None:
                return face
        return None
