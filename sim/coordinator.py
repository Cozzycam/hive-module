"""Coordinator — colony-wide orchestrator.

Owns the Colony state and the list of Chambers (modules). Per tick it
drives each chamber, then aggregates populations/brood counts into the
shared Colony object so every chamber sees consistent totals.

Phase 2 first cut:
  - `announce_module(attach_to, face, kind)` attaches a new empty
    chamber to an existing one's free face, wiring both entries.
  - `handoff(ant, from_id, to_id, face)` moves a worker across an
    active edge, placing it one cell inside the destination so it
    doesn't immediately bounce back.
  - `layout()` computes a (col, row) grid position for every module
    via BFS from the founding chamber, so the emulator has a stable
    place to draw each panel.

Still stubbed: KIND_OUTWORLD (food sources), KIND_TUNNEL, and the
per-module ESP-NOW broadcast pipeline.
"""

from sim.colony import Colony
from sim.chamber import Chamber, KIND_CHAMBER
from sim import protocol
import config as C


FOUNDING_ID = 'M0'


class Coordinator:
    def __init__(self):
        self.colony   = Colony()
        self.chambers = {}                   # module_id -> Chamber
        self.topology = {}                   # module_id -> {face: neighbour_id}
        self.tick_count = 0

        # Founding chamber bootstraps the colony.
        founding = Chamber(FOUNDING_ID, self.colony,
                           has_queen=True, kind=KIND_CHAMBER)
        self.chambers[FOUNDING_ID] = founding
        self.topology[FOUNDING_ID] = {}

    # ---- per-tick ----

    def tick(self):
        self.tick_count += 1

        # Snapshot worker positions before stepping, so the renderer
        # can interpolate between prev and current on sub-tick frames.
        for ch in self.chambers.values():
            for w in ch.workers:
                w.prev_x = w.x
                w.prev_y = w.y

        # Drive each chamber; hand me the ant when they cross an edge.
        for ch in list(self.chambers.values()):
            ch.tick(self)

        # Aggregate colony-wide counters
        pop = 0
        totals = {'egg': 0, 'larva': 0, 'pupa': 0}
        for ch in self.chambers.values():
            pop += len(ch.workers)
            cb = ch.count_brood()
            for k in totals:
                totals[k] += cb[k]
        self.colony.population   = pop
        self.colony.brood_counts = totals

    # ---- multi-module attachment ----

    def announce_module(self, attach_to, face, kind=KIND_CHAMBER):
        """Create a new chamber on `face` of an existing chamber.

        Returns (ok, new_id_or_error). Fails if the target face is
        already taken or the grid position is occupied by another
        module. The founding chamber always has a queen; all other
        chambers are empty.
        """
        if attach_to not in self.chambers:
            return False, f"unknown parent '{attach_to}'"
        if face not in C.FACE_DELTAS:
            return False, f"unknown face '{face}'"

        parent = self.chambers[attach_to]
        if parent.entries.get(face) is not None:
            return False, f"face '{face}' already connected"

        # Validate that the resulting grid position is free (and within
        # the layout bounds). The layout is BFS from the founding
        # chamber so we compute it fresh and check the candidate cell.
        positions = self.layout()
        pcol, prow = positions[attach_to]
        dcol, drow = C.FACE_DELTAS[face]
        ncol, nrow = pcol + dcol, prow + drow
        if not (0 <= ncol < C.LAYOUT_COLS and 0 <= nrow < C.LAYOUT_ROWS):
            return False, "target cell out of layout bounds"
        for mid, (col, row) in positions.items():
            if (col, row) == (ncol, nrow):
                return False, f"cell ({ncol},{nrow}) already occupied by {mid}"

        # Create + link the new chamber.
        new_id = f"M{len(self.chambers)}"
        new_chamber = Chamber(new_id, self.colony,
                              has_queen=False, kind=kind)
        self.chambers[new_id] = new_chamber

        # Wire both sides of the shared face.
        opp = C.FACE_OPPOSITE[face]
        parent.entries[face] = new_id
        parent.neighbors[face] = new_chamber
        new_chamber.entries[opp] = attach_to
        new_chamber.neighbors[opp] = parent

        # Update adjacency graph.
        self.topology.setdefault(attach_to, {})[face] = new_id
        self.topology[new_id] = {opp: attach_to}

        # Refresh home_face pointers — returning ants use these to
        # navigate back to the founding chamber through multi-hop
        # topologies without oscillating at intermediate junctions.
        self._recompute_home_faces()

        return True, new_id

    def _recompute_home_faces(self):
        """BFS from the founding chamber. For every other chamber,
        `home_face` is the face whose entry leads back along the
        shortest path to the founding chamber.
        """
        if FOUNDING_ID not in self.chambers:
            return
        self.chambers[FOUNDING_ID].home_face = None
        queue = [FOUNDING_ID]
        visited = {FOUNDING_ID}
        while queue:
            current = queue.pop(0)
            for face, neighbour_id in self.chambers[current].entries.items():
                if neighbour_id is None or neighbour_id in visited:
                    continue
                # Walking from `current` through `face` reaches
                # `neighbour_id`. The reverse face (opposite) on the
                # neighbour points back toward `current`, which is
                # closer to home.
                self.chambers[neighbour_id].home_face = C.FACE_OPPOSITE[face]
                visited.add(neighbour_id)
                queue.append(neighbour_id)

    # ---- ant handoff across active edges ----

    def handoff(self, ant, from_id, to_id, face):
        """Move `ant` from `from_id` onto the opposing face of `to_id`.

        The ant is placed one cell *inside* the destination chamber
        (rather than exactly on the entry cell) so the next tick won't
        immediately bounce it back across the edge.
        """
        if to_id not in self.chambers:
            return False
        dest = self.chambers[to_id]
        opp = C.FACE_OPPOSITE[face]
        ex, ey = C.ENTRY_POINTS[opp]

        # Step inward from the edge. FACE_DELTAS[opp] points outward
        # from `dest` toward `from_id`; invert it to step inside.
        dcol, drow = C.FACE_DELTAS[opp]
        ant.x = ex - dcol
        ant.y = ey - drow
        if not dest.in_bounds(ant.x, ant.y):
            # Fall back to the entry cell itself if the step-in lands
            # off-grid (shouldn't happen for the standard 60x40 grid).
            ant.x, ant.y = ex, ey
        # Reset interpolation so the ant doesn't "tween" across the gap.
        ant.prev_x = ant.x
        ant.prev_y = ant.y
        # Clear any stale task — target may be in the old chamber.
        from sim.ant import IDLE
        ant.state  = IDLE
        ant.target = None

        dest.workers.append(ant)
        return True

    # ---- layout ----

    def layout(self):
        """Compute a {module_id: (col, row)} grid position for every
        chamber via BFS from the founding chamber. Stable across ticks
        as long as topology is stable.
        """
        positions = {}
        if FOUNDING_ID not in self.chambers:
            return positions
        positions[FOUNDING_ID] = C.FOUNDING_POS

        queue = [FOUNDING_ID]
        visited = {FOUNDING_ID}
        while queue:
            mid = queue.pop(0)
            col, row = positions[mid]
            for face, neighbour_id in self.chambers[mid].entries.items():
                if neighbour_id is None or neighbour_id in visited:
                    continue
                dcol, drow = C.FACE_DELTAS[face]
                positions[neighbour_id] = (col + dcol, row + drow)
                visited.add(neighbour_id)
                queue.append(neighbour_id)
        return positions

    # ---- broadcasts ----

    def state_broadcast(self):
        """Build the COLONY_STATE message for UI / network consumers."""
        return protocol.colony_state(
            food        = round(self.colony.food_store, 1),
            population  = self.colony.population,
            brood_counts= dict(self.colony.brood_counts),
        )
