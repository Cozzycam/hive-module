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

import random

import config as C
from sim.queen import Queen
from sim.ant import Ant, TO_FOOD
from sim import brood as brood_mod
from sim.pheromones import PheromoneMap


# Kinds of chamber.
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
        self.neighbors = {face: None for face in C.ENTRY_POINTS}

        # Which face points home-ward (toward the founding chamber).
        # Used as a fallback when no to_home gradient exists yet.
        self.home_face = None

        # Physical food piles — {(x, y): amount}.
        self.food_cells = {}

        # Pheromone layers.
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
        if not self.in_bounds(x, y):
            return
        self.food_cells[(x, y)] = self.food_cells.get((x, y), 0.0) + amount

    def take_food(self, x, y, amount):
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

    def consume_food(self, x, y, amount, radius=None):
        """Consume up to `amount` from the nearest food pile to (x, y).
        If radius is given, only considers piles within that Manhattan
        distance. Returns actual amount consumed (may be less than
        requested). Empty piles are removed automatically."""
        if radius is None:
            radius = self.width + self.height
        best = None
        best_d = radius + 1
        for (fx, fy) in list(self.food_cells):
            if self.food_cells[(fx, fy)] <= 0.0:
                continue
            d = abs(fx - x) + abs(fy - y)
            if d < best_d:
                best_d = d
                best   = (fx, fy)
        if best is None:
            return 0.0
        return self.take_food(best[0], best[1], amount)

    def total_food(self):
        """Sum of all food piles in this chamber."""
        return sum(self.food_cells.values())

    # ---- pheromone deposits (corridor-mirrored) ----

    def deposit_home(self, x, y, amount):
        """Deposit to_home scent at (x, y), diffused across the 4
        cardinal neighbours at half intensity. A 3-cell-wide trail
        is robust to gradient-step sampling even when ants drift
        one cell off the original path.

        Every cell written (primary + 4 neighbours) goes through
        the per-cell helper so border entry cells still get mirrored
        to the neighbour chamber — otherwise cross-module trail
        continuity silently breaks for diffused deposits landing
        on a face entry."""
        self._deposit_home_cell(x, y, amount)
        half = amount * 0.5
        self._deposit_home_cell(x + 1, y, half)
        self._deposit_home_cell(x - 1, y, half)
        self._deposit_home_cell(x, y + 1, half)
        self._deposit_home_cell(x, y - 1, half)

    def deposit_food(self, x, y, amount):
        """Deposit to_food scent at (x, y). See deposit_home."""
        self._deposit_food_cell(x, y, amount)
        half = amount * 0.5
        self._deposit_food_cell(x + 1, y, half)
        self._deposit_food_cell(x - 1, y, half)
        self._deposit_food_cell(x, y + 1, half)
        self._deposit_food_cell(x, y - 1, half)

    def _deposit_home_cell(self, x, y, amount):
        """Single-cell to_home write + border mirror. Off-grid
        writes are silently ignored by the pheromone map."""
        self.pheromones.deposit_home(x, y, amount)
        self._mirror_deposit('home', x, y, amount)

    def _deposit_food_cell(self, x, y, amount):
        """Single-cell to_food write + border mirror."""
        self.pheromones.deposit_food(x, y, amount)
        self._mirror_deposit('food', x, y, amount)

    def _mirror_deposit(self, layer, x, y, amount):
        """If (x, y) is an active entry cell, copy the deposit to
        the neighbour chamber's opposite entry cell."""
        face = self._entry_face_at(x, y)
        if face is None:
            return
        neighbor = self.neighbors.get(face)
        if neighbor is None:
            return
        opp = C.FACE_OPPOSITE[face]
        nx, ny = C.ENTRY_POINTS[opp]
        if layer == 'home':
            neighbor.pheromones.deposit_home(nx, ny, amount)
        else:
            neighbor.pheromones.deposit_food(nx, ny, amount)

    def _entry_face_at(self, x, y):
        for face, entry_pos in C.ENTRY_POINTS.items():
            if entry_pos == (x, y):
                return face
        return None

    def nearest_food_within(self, x, y, radius):
        best   = None
        best_d = radius + 1
        for (fx, fy), amt in self.food_cells.items():
            if amt <= 0.0:
                continue
            d = abs(fx - x) + abs(fy - y)
            if d < best_d:
                best_d = d
                best   = (fx, fy)
        return best

    # ---- per-tick ----

    def tick(self, coordinator=None):
        # Pheromones decay before any fresh deposits.
        self.pheromones.decay()

        # Queen chamber beacon: always emit a strong to_home signal
        # at the queen's position. This is the equivalent of
        # JohnBuffer's "permanent marker" on the colony — ensures
        # the gradient always has a well-defined source at the nest.
        if self.queen is not None and self.queen.alive:
            self.pheromones.deposit_home(
                self.queen.x, self.queen.y,
                C.BASE_MARKER_INTENSITY,
            )

        # Queen
        if self.queen is not None:
            self.queen.tick(self)

        # Brood
        dead_brood = []
        hatched = []
        for b in self.brood:
            result = b.tick()
            if result == 'hatch':
                hatched.append(b)
            elif not b.alive:
                dead_brood.append(b)

        for b in hatched:
            is_nanitic = (self.colony.total_workers_born
                          < C.QUEEN_FOUNDING_EGG_CAP)
            self.workers.append(
                Ant(b.x, b.y, caste=b.caste, is_nanitic=is_nanitic),
            )
            self.colony.total_workers_born += 1
            self.brood.remove(b)
        for b in dead_brood:
            self.brood.remove(b)

        # Workers — shuffled so metabolism draws from food_store
        # in random order each tick (no first-in-list survival bias).
        random.shuffle(self.workers)
        dead_workers = []
        crossings = []
        for w in self.workers:
            w.tick(self)
            if not w.alive:
                dead_workers.append(w)
                continue
            crossing = self._check_edge_crossing(w)
            if crossing is not None:
                crossings.append((w, crossing))

        for w in dead_workers:
            # Drop carried food as a small pile at death location.
            if w.food_carried > 0:
                self.add_food(w.x, w.y, w.food_carried)
            self.workers.remove(w)

        # Hand crossings off to the coordinator
        if coordinator is not None and crossings:
            for w, face in crossings:
                neighbour_id = self.entries.get(face)
                if neighbour_id is None:
                    continue
                # Suppress empty TO_FOOD ants bouncing back through
                # home face — they should be exploring outward, not
                # retreating empty-handed. Belt-and-braces; the
                # gradient should prevent this naturally.
                if (face == self.home_face
                        and w.state == TO_FOOD
                        and w.food_carried <= 0):
                    continue
                if w in self.workers:
                    self.workers.remove(w)
                coordinator.handoff(w, self.module_id, neighbour_id, face)

    def _check_edge_crossing(self, worker):
        pos = (worker.x, worker.y)
        for face, entry_pos in C.ENTRY_POINTS.items():
            if pos == entry_pos and self.entries.get(face) is not None:
                return face
        return None
