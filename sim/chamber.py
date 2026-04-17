"""A Chamber is one physical module's local simulation space.

Holds the grid, the queen (if this is the founding chamber), brood, and
workers. Chambers are driven by the Coordinator's per-tick loop. Only
the chamber knows about per-cell geometry; higher layers treat it as a
black box with a population of entities.

A Chamber also tracks its neighbour on each of its four faces via the
`entries` dict (face -> neighbour module_id or None). When a worker
steps onto an entry cell on an active face the chamber flags it for
handoff and the Coordinator moves the worker to the opposite face of the
neighbour on the same tick.
"""

import random

import config as C
from sim.queen import Queen
from sim.lil_guy import LilGuy, TO_FOOD, TO_HOME
from sim import brood as brood_mod
from sim import events
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
        self.workers = []       # list[LilGuy]

        # face -> neighbour module_id (None = no neighbour attached).
        self.entries = {face: None for face in C.ENTRY_POINTS}

        # face -> neighbour Chamber reference (None = no neighbour).
        self.neighbors = {face: None for face in C.ENTRY_POINTS}

        # Which face points home-ward (toward the founding chamber).
        # Used as a fallback when no to_home gradient exists yet.
        self.home_face = None

        # Physical food piles — {(x, y): amount}.
        self.food_cells = {}

        # Brood cannibalism cooldown (ticks until next allowed).
        self.cannibalism_cooldown = 0

        # Food delivery signal — set when a gatherer deposits food.
        # Idle workers check this for recruitment (not pheromone, which
        # lingers and causes false triggers from stored food).
        self.food_delivery_signal = 0

        # Pheromone layers.
        self.pheromones = PheromoneMap(self.width, self.height)

        # Transient per-tick — set by coordinator before tick().
        self._event_bus = None
        self._tick = 0

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
        is robust to gradient-step sampling even when workers drift
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
        # Bind transient event context for this tick.
        if coordinator is not None:
            self._event_bus = coordinator.event_bus
            self._tick = coordinator.tick_count
        else:
            self._event_bus = None
            self._tick = 0

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
            elif result == 'egg_to_larva':
                self._emit(events.young_hatched(
                    self._tick, 'egg', 'larva'))
            elif result == 'larva_to_pupa':
                self._emit(events.young_hatched(
                    self._tick, 'larva', 'pupa'))
            elif result == 'died':
                self._emit(events.young_died(self._tick))
            elif not b.alive:
                dead_brood.append(b)

        for b in hatched:
            is_pioneer = (self.colony.total_workers_born
                          < C.QUEEN_FOUNDING_EGG_CAP)
            self.workers.append(
                LilGuy(b.x, b.y, role=b.role, is_pioneer=is_pioneer),
            )
            self.colony.total_workers_born += 1
            self._emit(events.young_hatched(
                self._tick, 'pupa', 'worker'))
            self.brood.remove(b)
        for b in dead_brood:
            self._emit(events.young_died(self._tick))
            self.brood.remove(b)

        # Starvation cascade stage 1 — cull hungry larvae under
        # high food pressure. Larvae unfed for ~500+ ticks are the
        # first biological casualties.
        pressure = self.colony.food_pressure()
        if pressure > C.FAMINE_BROOD_CULL_PRESSURE:
            famine_dead = [
                b for b in self.brood
                if (b.stage == brood_mod.LARVA
                    and b.alive
                    and b.hunger > C.FAMINE_BROOD_CULL_HUNGER)
            ]
            for b in famine_dead:
                self._emit(events.young_died(self._tick))
                self.brood.remove(b)

        # Cooldown ticks.
        if self.cannibalism_cooldown > 0:
            self.cannibalism_cooldown -= 1
        if self.food_delivery_signal > 0:
            self.food_delivery_signal -= 1

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

        # Proximity interactions between lil guys.
        self._detect_proximity_interactions()

        for w in dead_workers:
            # Drop carried food as a small pile at death location.
            if w.food_carried > 0:
                self.add_food(w.x, w.y, w.food_carried)
            self._emit(events.lil_guy_died(self._tick))
            self.workers.remove(w)

        # Hand crossings off to the coordinator
        if coordinator is not None and crossings:
            for w, face in crossings:
                neighbour_id = self.entries.get(face)
                if neighbour_id is None:
                    continue
                # Suppress empty TO_FOOD workers bouncing back through
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

    # ---- event helpers ----

    def _emit(self, event):
        if self._event_bus is not None:
            self._event_bus.emit(event)

    def _detect_proximity_interactions(self):
        """Scan for adjacent lil guy pairs and emit interaction events.

        Uses a spatial hash so cost is O(n) build + O(k) where k is the
        number of actual adjacent pairs, not O(n^2).
        """
        bus = self._event_bus
        if bus is None or len(self.workers) < 2:
            return

        # Build spatial index: (x, y) -> [worker index]
        grid = {}
        for i, w in enumerate(self.workers):
            if not w.alive:
                continue
            key = (w.x, w.y)
            if key in grid:
                grid[key].append(i)
            else:
                grid[key] = [i]

        checked = set()
        tick = self._tick
        radius = C.PROXIMITY_DETECTION_RADIUS

        for (cx, cy), indices in grid.items():
            # Same-cell pairs
            for ai in range(len(indices)):
                for bi in range(ai + 1, len(indices)):
                    self._maybe_interact(
                        indices[ai], indices[bi], tick, bus, checked)
            # Adjacent cells (4 cardinal) — only if radius >= 1
            if radius >= 1:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nkey = (cx + dx, cy + dy)
                    if nkey not in grid:
                        continue
                    for ai in indices:
                        for bi in grid[nkey]:
                            self._maybe_interact(
                                ai, bi, tick, bus, checked)

    def _maybe_interact(self, ai, bi, tick, bus, checked):
        """Roll for an interaction between two lil guys (by index).
        Emits start+end on the same tick with duration_hint for the
        renderer. The sim stays stateless with respect to interactions.
        """
        pair = (min(ai, bi), max(ai, bi))
        if pair in checked:
            return
        checked.add(pair)

        a = self.workers[ai]
        b = self.workers[bi]

        # Food sharing: one carrying, the other not.
        if ((a.food_carried > 0) != (b.food_carried > 0)):
            if random.random() < C.PROXIMITY_FOOD_SHARE_CHANCE:
                pid = bus.next_pair_id()
                bus.emit(events.interaction_started(
                    tick, pid, events.FOOD_SHARING,
                    duration_hint=C.FOOD_SHARE_DURATION_TICKS))
                bus.emit(events.interaction_ended(tick, pid))
                return

        # General greeting
        if random.random() < C.PROXIMITY_GREETING_CHANCE:
            pid = bus.next_pair_id()
            bus.emit(events.interaction_started(
                tick, pid, events.GREETING,
                duration_hint=C.GREETING_DURATION_TICKS))
            bus.emit(events.interaction_ended(tick, pid))
