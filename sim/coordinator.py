"""Coordinator — colony-wide orchestrator.

Owns the Colony state and the list of Chambers (modules). Per tick it
drives each chamber, then aggregates populations/brood counts into the
shared Colony object so every chamber sees consistent totals.

Key change from the old system: handoff() now preserves ant state
(TO_FOOD / TO_HOME), food_carried, and steps_walked across chamber
boundaries. The ant continues its mission in the new chamber — the
marker gradient guides it without needing a state reset.
"""

import math

from sim.colony import Colony
from sim.chamber import Chamber, KIND_CHAMBER
from sim.ant import TO_FOOD, TO_HOME
from sim import protocol
import config as C


FOUNDING_ID = 'M0'


class Coordinator:
    def __init__(self):
        self.colony   = Colony()
        self.chambers = {}
        self.topology = {}
        self.tick_count = 0

        founding = Chamber(FOUNDING_ID, self.colony,
                           has_queen=True, kind=KIND_CHAMBER)
        self.chambers[FOUNDING_ID] = founding
        self.topology[FOUNDING_ID] = {}

    # ---- per-tick ----

    def tick(self):
        self.tick_count += 1

        for ch in self.chambers.values():
            for w in ch.workers:
                w.prev_x = w.x
                w.prev_y = w.y

        for ch in list(self.chambers.values()):
            ch.tick(self)

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
        if attach_to not in self.chambers:
            return False, f"unknown parent '{attach_to}'"
        if face not in C.FACE_DELTAS:
            return False, f"unknown face '{face}'"

        parent = self.chambers[attach_to]
        if parent.entries.get(face) is not None:
            return False, f"face '{face}' already connected"

        positions = self.layout()
        pcol, prow = positions[attach_to]
        dcol, drow = C.FACE_DELTAS[face]
        ncol, nrow = pcol + dcol, prow + drow
        if not (0 <= ncol < C.LAYOUT_COLS and 0 <= nrow < C.LAYOUT_ROWS):
            return False, "target cell out of layout bounds"
        for mid, (col, row) in positions.items():
            if (col, row) == (ncol, nrow):
                return False, f"cell ({ncol},{nrow}) already occupied by {mid}"

        new_id = f"M{len(self.chambers)}"
        new_chamber = Chamber(new_id, self.colony,
                              has_queen=False, kind=kind)
        self.chambers[new_id] = new_chamber

        opp = C.FACE_OPPOSITE[face]
        parent.entries[face] = new_id
        parent.neighbors[face] = new_chamber
        new_chamber.entries[opp] = attach_to
        new_chamber.neighbors[opp] = parent

        self.topology.setdefault(attach_to, {})[face] = new_id
        self.topology[new_id] = {opp: attach_to}

        self._recompute_home_faces()
        return True, new_id

    def _recompute_home_faces(self):
        """BFS from founding chamber — sets home_face for every
        non-founding chamber. Used as a fallback when no to_home
        gradient exists yet (early colony, sparse trails)."""
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
                self.chambers[neighbour_id].home_face = C.FACE_OPPOSITE[face]
                visited.add(neighbour_id)
                queue.append(neighbour_id)

    # ---- ant handoff across active edges ----

    def handoff(self, ant, from_id, to_id, face):
        """Move ant across a chamber boundary.

        Key design: the ant's state (TO_FOOD / TO_HOME), food_carried,
        and steps_walked are ALL preserved. The marker gradient is
        continuous across borders (entry cell mirroring ensures this),
        so the ant picks up the gradient on the other side naturally.

        What IS reset:
          - Position: one cell inside the destination (not on the
            entry cell, to prevent immediate re-crossing).
          - prev_x/prev_y: for clean interpolation.
          - facing: set to point inward from the entry face.
          - target: cleared (it was a cell in the old chamber).
          - last_dx/last_dy: set to match new facing so momentum
            walk doesn't try to walk back the way it came.

        Handoff-time deposit: the ant is placed one cell inside
        the destination (to prevent instant re-crossing), which
        normally means the placement cell never receives a primary
        pheromone deposit — its only signal is half-strength
        diffusion from the NEXT cell the ant visits. That creates
        a local-minimum valley in the gradient exactly at the
        border, and gradient-walking ants get trapped oscillating
        one cell west of it. We fix this by laying a deposit
        equivalent to what the ant would have dropped had it
        walked onto the placement cell naturally — matching the
        layer and intensity formula used by _do_to_food/_do_to_home.
        """
        if to_id not in self.chambers:
            return False
        dest = self.chambers[to_id]
        opp = C.FACE_OPPOSITE[face]
        ex, ey = C.ENTRY_POINTS[opp]

        # Step inward from the entry.
        dcol, drow = C.FACE_DELTAS[opp]
        ant.x = ex - dcol
        ant.y = ey - drow
        if not dest.in_bounds(ant.x, ant.y):
            ant.x, ant.y = ex, ey

        # Reset interpolation.
        ant.prev_x = ant.x
        ant.prev_y = ant.y

        # Face inward — the negative of the outward-pointing delta.
        ant.facing_dx = -dcol
        ant.facing_dy = -drow
        ant.last_dx   = -dcol
        ant.last_dy   = -drow

        # Clear stale target (was in old chamber).
        ant.target = None

        # State, food_carried, steps_walked are PRESERVED.
        # The ant continues its mission in the new chamber.

        # Fill the handoff gap so the gradient is continuous.
        # Goes through dest.deposit_home/food (not pheromones.*
        # directly) so the diffused cross-shape is written and
        # mirroring still fires for the adjacent entry cell.
        #
        # Intensity uses steps_walked - 1 rather than steps_walked.
        # The subtraction compensates for the move_cooldown-length
        # gap between this deposit and the next in-chamber deposit:
        # without the -1 bias, the handoff cell sits a few ticks
        # older than its neighbour inward, and decay alone is
        # enough to turn it back into a local-minimum valley.
        effective_steps = max(0, ant.steps_walked - 1)
        intensity = C.BASE_MARKER_INTENSITY * math.exp(
            -C.MARKER_STEP_DECAY * effective_steps
        )
        if ant.state == TO_HOME and ant.food_carried > 0:
            dest.deposit_food(ant.x, ant.y, intensity)
        elif ant.state == TO_FOOD:
            dest.deposit_home(ant.x, ant.y, intensity)

        dest.workers.append(ant)
        return True

    # ---- layout ----

    def layout(self):
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
        return protocol.colony_state(
            food        = round(self.colony.food_store, 1),
            population  = self.colony.population,
            brood_counts= dict(self.colony.brood_counts),
        )
