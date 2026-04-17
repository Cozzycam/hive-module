"""Worker — JohnBuffer-inspired marker-following gathering.

States:
  IDLE       — no current task. Random walk near queen, picks up
               domestic duties or goes scouting.
  TEND_QUEEN — walking to the queen to feed her (in-chamber only).
  TEND_BROOD — walking to a hungry larva to feed it (in-chamber only).
  TO_FOOD    — searching for food. Deposits to_home markers whose
               intensity decays with distance from colony. Follows
               to_food gradient laid by returning workers. On finding
               food, flips 180° and switches to TO_HOME.
  TO_HOME    — carrying food back. Deposits to_food markers whose
               intensity decays with distance from food. Follows
               to_home gradient laid by outbound workers. On reaching
               the queen, drops cargo and switches to IDLE.

Movement engine:
  Workers have float (x, y) positions and move fractionally each tick
  toward a target_cell (int, int). Cell derivation: int(floor(x/y)).
  Decision handlers only fire when target_cell is None (arrived).

  Workers read the 4 cardinal-neighbour marker values directly and step
  toward the strongest. This is the discrete-grid equivalent of
  JohnBuffer's continuous-space random-cone sampling — cheaper,
  deterministic, and far more reliable because we can't miss a
  trail cell the worker is literally adjacent to. If every neighbour
  reads zero the worker falls through to a momentum-biased random
  walk (see _persistent_forward_step) so lost workers still explore.

  Trails are diffused at deposit time (Chamber.deposit_home/food
  writes the primary cell plus its 4 cardinal neighbours at half
  intensity), so a 1-cell walking path becomes a 3-cell-wide trail
  and a worker drifting slightly off the centre line still picks it
  up on the next step.

  Distance from source is encoded in deposit intensity via
  BASE_MARKER_INTENSITY * exp(-MARKER_STEP_DECAY * steps_walked).
  Cells near the colony have strong to_home markers; cells near food
  have strong to_food markers. Gradients point the right way by
  construction — no pathfinding, no corridor sampling, no BFS.

  Pheromone deposits happen on cell entry (tracked by last_cell),
  not every tick, so trail density matches actual grid traversal.
"""

import random
import math

import config as C
from sim import brood as brood_mod
from sim import events


# ---- state tags ----

IDLE        = 'idle'
TEND_QUEEN  = 'tend_queen'
TEND_BROOD  = 'tend_brood'
TO_FOOD     = 'to_food'
TO_HOME     = 'to_home'
CANNIBALIZE = 'cannibalize'

# Legacy aliases — renderer and module_panel reference these.
OUTBOUND    = TO_FOOD
RETURNING   = TO_HOME
FORAGE      = TO_FOOD


class LilGuy:
    __slots__ = (
        'x', 'y', 'prev_x', 'prev_y', 'state', 'target',
        'target_cell', 'speed', 'last_cell',
        'age', 'alive',
        'role', 'move_ticks', 'sense_radius', 'carry_amount',
        'facing_dx', 'facing_dy', 'food_carried',
        'last_dx', 'last_dy',
        'steps_walked', 'ticks_away', 'stall_ticks',
        'idle_cooldown', 'chamber_steps',
        'hunger', 'max_age', 'metabolism',
        'is_pioneer',
    )

    def __init__(self, x, y, role=None, is_pioneer=False):
        self.x             = float(x) + 0.5
        self.y             = float(y) + 0.5
        self.prev_x        = self.x
        self.prev_y        = self.y
        self.state         = IDLE
        self.target        = None
        self.target_cell   = None   # next cell to walk toward, (int, int) or None
        self.age           = 0
        self.alive         = True

        # Random initial facing so workers scatter naturally on spawn.
        self.facing_dx     = float(random.choice((-1, 1)))
        self.facing_dy     = 0.0
        self.last_dx       = self.facing_dx
        self.last_dy       = 0.0

        self.food_carried  = 0.0

        # Steps since last source (colony or food). Used to compute
        # deposit intensity: markers weaken with distance from source.
        self.steps_walked  = 0

        # Ticks spent outside the queen chamber. When this exceeds
        # RETURN_HOME_TICKS the worker is forced into TO_HOME so it
        # doesn't get permanently stuck in a non-queen module.
        self.ticks_away    = 0

        # Consecutive movement ticks where the worker didn't change
        # position (stalled at the peak of an exhausted-food
        # gradient). After STALL_THRESHOLD_TICKS the worker ignores
        # the gradient and explores randomly.
        self.stall_ticks   = 0

        # Cooldown before an IDLE worker re-rolls the scouting
        # decision. Prevents the wave-departure problem where
        # every worker re-rolls every tick and all leave at once.
        self.idle_cooldown = 0

        # Steps taken in the CURRENT chamber. Resets on chamber
        # crossing so the worker explores each new chamber before
        # seeking exits. Separate from steps_walked which drives
        # pheromone deposit intensity.
        self.chamber_steps = 0

        # Hunger — accumulates when food_store can't cover
        # metabolism. Worker dies at WORKER_STARVE_THRESHOLD.
        self.hunger        = 0.0

        # Role-dependent params — baked at spawn time.
        self.role = role if role is not None else C.ROLE_MINOR
        params = C.ROLE_PARAMS[self.role]
        self.move_ticks    = params['move_ticks']
        self.sense_radius  = params['sense_radius']
        self.carry_amount  = params['carry_amount']
        self.metabolism    = params['metabolism']
        self.speed         = params['speed']

        # Lifespan — pioneers (first founding brood) are
        # shorter-lived than regular workers of the same role.
        self.is_pioneer = is_pioneer
        if is_pioneer:
            lo, hi = C.WORKER_LIFESPAN_PIONEER
        else:
            lo, hi = params['lifespan']
        self.max_age = random.randint(lo, hi)

        # Last cell entered — pheromone deposits are gated on cell entry.
        self.last_cell = (int(x), int(y))

    # ================================================================
    #  Position helpers
    # ================================================================

    def cell(self):
        """Current grid cell as (int, int), derived from float position."""
        return int(math.floor(self.x)), int(math.floor(self.y))

    def _set_target_cell(self, cx, cy, chamber):
        """Set the next cell to walk toward. Returns True if valid."""
        if not chamber.in_bounds(cx, cy):
            return False
        self.target_cell = (cx, cy)
        return True

    def _advance_toward_target(self, chamber):
        """Move fractionally toward target_cell center. Called every tick."""
        if self.target_cell is None:
            return
        # Target is center of the target cell
        tx = self.target_cell[0] + 0.5
        ty = self.target_cell[1] + 0.5
        dx = tx - self.x
        dy = ty - self.y
        dist = math.sqrt(dx * dx + dy * dy)
        if dist < C.ARRIVAL_THRESHOLD:
            # Snap to center, mark arrival
            self.x, self.y = tx, ty
            new_cell = self.target_cell
            if new_cell != self.last_cell:
                self._on_enter_cell(new_cell, chamber)
                self.last_cell = new_cell
            self.target_cell = None
            return
        # Move toward target, don't overshoot
        step = min(self.speed, dist)
        self.x += (dx / dist) * step
        self.y += (dy / dist) * step
        # Update facing from velocity
        self.facing_dx = dx / dist
        self.facing_dy = dy / dist
        self.last_dx = self.facing_dx
        self.last_dy = self.facing_dy
        # Check cell entry mid-transit
        new_cell = self.cell()
        if new_cell != self.last_cell:
            self._on_enter_cell(new_cell, chamber)
            self.last_cell = new_cell

    def _on_enter_cell(self, new_cell, chamber):
        """Called when floor'd cell changes. Pheromone deposits happen here."""
        cx, cy = new_cell
        if self.state == TO_FOOD:
            intensity = C.BASE_MARKER_INTENSITY * math.exp(
                -C.MARKER_STEP_DECAY * self.steps_walked)
            chamber.deposit_home(cx, cy, intensity)
            self.steps_walked += 1
            self.chamber_steps += 1
        elif self.state == TO_HOME:
            if self.food_carried > 0:
                intensity = C.BASE_MARKER_INTENSITY * math.exp(
                    -C.MARKER_STEP_DECAY * self.steps_walked)
                chamber.deposit_food(cx, cy, intensity)
            self.steps_walked += 1

    # ================================================================
    #  Per-tick entry point
    # ================================================================

    def tick(self, chamber):
        if not self.alive:
            return

        self.age += 1

        # Natural death — old age.
        if self.age >= self.max_age:
            self.alive = False
            chamber._emit(events.lil_guy_died(chamber._tick))
            return

        # Metabolism — draw from colony food_store, or nibble from
        # own cargo if the store is empty.
        scale = C.metabolic_scale_factor(chamber.colony.population)
        drain = self.metabolism * scale
        store = chamber.colony.food_store
        if store >= drain:
            chamber.colony.food_store -= drain
            if self.hunger > 0:
                self.hunger = max(0.0, self.hunger - drain)
        elif self.food_carried >= drain:
            # Store empty but carrying food — eat from cargo.
            self.food_carried -= drain
            if self.hunger > 0:
                self.hunger = max(0.0, self.hunger - drain)
        else:
            self.hunger += C.WORKER_HUNGER_RATE
            if self.hunger >= C.WORKER_STARVE_THRESHOLD:
                self.alive = False
                chamber._emit(events.lil_guy_died(chamber._tick))
                return

        # Track time away from the queen chamber.
        if chamber.queen is not None:
            self.ticks_away = 0
        else:
            self.ticks_away += 1

        # Return-home timer — after too long in a non-queen chamber,
        # interrupt whatever the worker is doing and head home.
        if (self.ticks_away >= C.RETURN_HOME_TICKS
                and self.state != TO_HOME):
            self.state        = TO_HOME
            self.target       = None
            self.target_cell  = None
            self.steps_walked = 0
            self.facing_dx    = -self.facing_dx
            self.facing_dy    = -self.facing_dy

        # Movement phase — every tick
        self._advance_toward_target(chamber)

        # Decision phase — only when arrived (no pending target)
        if self.state == IDLE:
            if self.idle_cooldown > 0:
                self.idle_cooldown -= 1
            else:
                self._pick_task(chamber)
        elif not self._target_still_valid(chamber):
            self._pick_task(chamber)

        if self.target_cell is None:
            if self.state == TEND_BROOD:
                self._do_tend_brood(chamber)
            elif self.state == TEND_QUEEN:
                self._do_tend_queen(chamber)
            elif self.state == CANNIBALIZE:
                self._do_cannibalize(chamber)
            elif self.state == TO_FOOD:
                self._do_to_food(chamber)
            elif self.state == TO_HOME:
                self._do_to_home(chamber)
            else:
                self._do_idle(chamber)

    # ================================================================
    #  Task selection
    # ================================================================

    def _pick_task(self, chamber):
        """Priority: cargo delivery > queen (famine) > domestic > gather."""

        # Food in crop must be delivered first.
        if self.food_carried > 0:
            self.state  = TO_HOME
            self.target = None
            self.target_cell = None
            return

        # Non-queen chamber — head home.
        if chamber.queen is None:
            self.state        = TO_HOME
            self.target       = None
            self.target_cell  = None
            self.steps_walked = 0
            return

        colony   = chamber.colony
        pressure = colony.food_pressure()
        queen    = chamber.queen
        has_food = colony.food_store >= C.LARVA_FEED_AMOUNT

        # Stage 2 override — under severe famine, workers feed the
        # queen first if her hunger is critical, before anything else.
        if (pressure > C.FAMINE_SLOWDOWN_PRESSURE
                and queen is not None
                and queen.hunger > C.QUEEN_PRIORITY_HUNGER
                and has_food
                and self._within_sense(queen.x, queen.y)):
            self.state  = TEND_QUEEN
            self.target = (queen.x, queen.y)
            self.target_cell = None
            return

        # Domestic: feed queen (normal priority).
        if (queen is not None
                and queen.needs_feeding()
                and has_food
                and self._within_sense(queen.x, queen.y)):
            self.state  = TEND_QUEEN
            self.target = (queen.x, queen.y)
            self.target_cell = None
            return

        # Stage 2 — under severe famine, workers stop tending brood
        # (self-preservation). Also trigger brood cannibalism.
        if pressure > C.FAMINE_SLOWDOWN_PRESSURE:
            # Brood cannibalism: convert doomed larvae back to food.
            if (pressure >= C.BROOD_CANNIBALISM_PRESSURE
                    and colony.food_store < C.LARVA_FEED_AMOUNT
                    and chamber.cannibalism_cooldown <= 0):
                victim = self._least_invested_larva(chamber)
                if victim is not None:
                    self.state  = CANNIBALIZE
                    self.target = (victim.x, victim.y)
                    self.target_cell = None
                    chamber.cannibalism_cooldown = C.BROOD_CANNIBALISM_COOLDOWN
                    return
        else:
            # Normal domestic: feed larvae — but maintain a gatherer
            # floor so brood tending doesn't completely starve gathering
            # income.  Without this, all workers go domestic when larvae
            # hatch, income drops to zero, and food_store crashes.
            larva = self._nearest_hungry_larva(chamber)
            if larva is not None and has_food:
                total_pop = colony.population
                target_frac = colony.target_gatherer_fraction()
                min_gatherers = max(2, int(total_pop * target_frac))
                if colony.gatherer_count >= min_gatherers:
                    self.state  = TEND_BROOD
                    self.target = (larva.x, larva.y)
                    self.target_cell = None
                    return
                # Not enough gatherers — fall through to gathering.

        # Recruitment signal — a returning gatherer has laid to_food
        # pheromone in the nest. Idle workers that detect the trail
        # follow it immediately, overriding the gatherer fraction cap.
        # This is the core recruitment mechanism.
        if self._detects_food_trail(chamber):
            self.state         = TO_FOOD
            self.target        = None
            self.target_cell   = None
            self.steps_walked  = 0
            self.chamber_steps = 0
            return

        # Dynamic gatherer regulation — baseline fraction from
        # pressure, with recovery bounce override.
        target_frac = colony.target_gatherer_fraction()
        total_pop = colony.population
        if total_pop > 0 and colony.gatherer_count / total_pop >= target_frac:
            # Enough gatherers out — stay home, but reconsider
            # faster under famine so workers don't idle while starving.
            if pressure > C.FAMINE_SLOWDOWN_PRESSURE:
                cd = random.randint(10, 30)
            else:
                cd = random.randint(
                    C.IDLE_RECONSIDER_MIN, C.IDLE_RECONSIDER_MAX,
                )
            self.state         = IDLE
            self.target        = None
            self.target_cell   = None
            self.idle_cooldown = cd
            return

        # Go gather.
        self.state         = TO_FOOD
        self.target        = None
        self.target_cell   = None
        self.steps_walked  = 0
        self.chamber_steps = 0

    # ================================================================
    #  Gathering: TO_FOOD (outbound, searching for food)
    # ================================================================

    def _do_to_food(self, chamber):
        # Scout patience — give up and head home after too long.
        if self.steps_walked > C.SCOUT_PATIENCE_TICKS:
            self.state         = TO_HOME
            self.target        = None
            self.target_cell   = None
            self.steps_walked  = 0
            self.facing_dx     = -self.facing_dx
            self.facing_dy     = -self.facing_dy
            return

        # Check for adjacent food — pick it up.
        pile = self._food_pile_adjacent(chamber)
        if pile is not None:
            px, py = pile
            taken = chamber.take_food(px, py, self.carry_amount)
            if taken > 0:
                self.food_carried  = taken
                self.state         = TO_HOME
                self.target        = None
                self.target_cell   = None
                self.steps_walked  = 0
                # Flip facing 180° — "forward" now points home.
                self.facing_dx = -self.facing_dx
                self.facing_dy = -self.facing_dy
                return

        # Movement decision (in priority order):
        # 1. Walk toward a visible food pile within sense radius.
        # 2. Follow to_food gradient (unless stalled at an
        #    exhausted site — skip to random walk).
        # 3. Entry attraction / momentum random walk.
        cx, cy = self.cell()
        visible_pile = chamber.nearest_food_within(
            cx, cy, self.sense_radius,
        )
        if visible_pile is not None:
            self._step_toward_cell(visible_pile, chamber)
            self.stall_ticks = 0
        elif self.stall_ticks < C.STALL_THRESHOLD_TICKS:
            step = self._sample_markers(chamber, 'food')
            if step is not None:
                dx, dy = step
                cx, cy = self.cell()
                self._set_target_cell(cx + dx, cy + dy, chamber)
            else:
                self._explore_or_wander(chamber)
        else:
            self._explore_or_wander(chamber)

        # Update stall counter — increments when following a food
        # gradient with no actual food in sight. This catches both
        # true stalls AND oscillation at exhausted peaks.
        if visible_pile is not None:
            pass  # already reset above
        elif self._sample_markers(chamber, 'food') is not None:
            # On a food trail but no food visible — counting.
            self.stall_ticks += 1
        else:
            # No trail at all — exploring freely, not stalled.
            self.stall_ticks = 0

    # ================================================================
    #  Gathering: TO_HOME (returning with food)
    # ================================================================

    def _do_to_home(self, chamber):
        if chamber.queen is not None:
            # In the queen chamber — walk to queen, dump food into
            # the colony's abstract food store.
            qx, qy = chamber.queen.x, chamber.queen.y
            cx, cy = self.cell()
            if abs(qx - cx) + abs(qy - cy) <= 1:
                chamber._emit(events.food_delivered(
                    chamber._tick, cx, cy, self.food_carried))
                chamber.colony.food_store += self.food_carried
                self.food_carried  = 0.0
                # Signal nestmates — a gatherer just delivered food.
                chamber.food_delivery_signal = 200
                self.state         = IDLE
                self.target        = None
                self.target_cell   = None
                self.steps_walked  = 0
                # Gathering wear — completed trips age the worker.
                self.age += random.randint(*C.GATHERING_TRIP_WEAR)
                self.facing_dx = -self.facing_dx
                self.facing_dy = -self.facing_dy
                return
            self._step_toward_cell((qx, qy), chamber)
        else:
            # Not in the queen chamber — follow to_home gradient.
            step = self._sample_markers(chamber, 'home')
            if step is not None:
                dx, dy = step
                cx, cy = self.cell()
                self._set_target_cell(cx + dx, cy + dy, chamber)
            else:
                home_face = chamber.home_face
                if home_face is not None:
                    entry = C.ENTRY_POINTS[home_face]
                    self._step_toward_cell(entry, chamber)
                else:
                    self._persistent_forward_step(chamber)

    # ================================================================
    #  Marker sampling — the core engine (JohnBuffer-style)
    # ================================================================

    def _sample_markers(self, chamber, layer):
        """Read the 4 cardinal-neighbour marker values on the given
        layer and return (dx, dy) toward the strongest neighbour,
        or None if every neighbour reads zero.

        This is a direct gradient step, not random sampling. On a
        discrete grid with four known neighbours, deterministic
        gradient-following is both cheaper and dramatically more
        reliable than JohnBuffer-style random cone sampling — which
        makes sense for his continuous-space sim but is the wrong
        primitive here.

        Gradient direction is correct by construction:
          - to_home markers are strongest near the queen (source)
            and weakest far from it, so "strongest neighbour"
            always points uphill toward the nest.
          - to_food markers are strongest near food (source), so
            "strongest neighbour" points uphill toward food.

        Ties break toward the worker's current facing so committed
        walkers stay committed; remaining ties break randomly so
        workers don't all queue into the same cell.
        """
        cx, cy = self.cell()
        read_fn = (chamber.pheromones.home if layer == 'home'
                   else chamber.pheromones.food)
        neighbours = (
            ( 1,  0, read_fn(cx + 1, cy)),
            (-1,  0, read_fn(cx - 1, cy)),
            ( 0,  1, read_fn(cx, cy + 1)),
            ( 0, -1, read_fn(cx, cy - 1)),
        )

        best_val = 0.0
        best_picks = []
        for dx, dy, val in neighbours:
            if val > best_val:
                best_val = val
                best_picks = [(dx, dy)]
            elif val == best_val and val > 0:
                best_picks.append((dx, dy))

        if best_val <= 0:
            return None
        if len(best_picks) == 1:
            return best_picks[0]

        # Quantize facing to cardinal for tie-break
        if abs(self.facing_dx) >= abs(self.facing_dy):
            facing = (1 if self.facing_dx > 0 else -1, 0)
        else:
            facing = (0, 1 if self.facing_dy > 0 else -1)
        if facing in best_picks:
            return facing
        return random.choice(best_picks)

    # ================================================================
    #  Domestic tasks — two-phase: pick up food, then deliver
    # ================================================================

    def _do_tend_brood(self, chamber):
        """Walk to target larva, deduct from food_store, feed it."""
        tx, ty = self.target
        cx, cy = self.cell()
        if abs(tx - cx) + abs(ty - cy) <= 1:
            feed_amt = C.LARVA_FEED_AMOUNT
            store = chamber.colony.food_store
            if store >= feed_amt:
                for b in chamber.brood:
                    if (b.x, b.y) == (tx, ty) and b.stage == brood_mod.LARVA:
                        if not b.needs_feeding():
                            break  # already fed by another worker
                        chamber.colony.food_store -= feed_amt
                        b.feed(feed_amt)
                        self._emit_interaction(
                            chamber, events.TENDING_YOUNG,
                            C.GREETING_DURATION_TICKS)
                        break
            self.state         = IDLE
            self.target        = None
            self.target_cell   = None
            self.idle_cooldown = random.randint(20, 40)
            return
        self._step_toward_cell((tx, ty), chamber)

    def _do_tend_queen(self, chamber):
        """Walk to queen, deduct from food_store, reduce her hunger."""
        tx, ty = self.target
        cx, cy = self.cell()
        if abs(tx - cx) + abs(ty - cy) <= 1:
            feed_amt = C.LARVA_FEED_AMOUNT
            store = chamber.colony.food_store
            if store >= feed_amt:
                chamber.colony.food_store -= feed_amt
                if chamber.queen is not None:
                    chamber.queen.hunger = max(
                        0.0, chamber.queen.hunger - feed_amt,
                    )
                self._emit_interaction(
                    chamber, events.TENDING_QUEEN,
                    C.GREETING_DURATION_TICKS)
            self.state         = IDLE
            self.target        = None
            self.target_cell   = None
            self.idle_cooldown = random.randint(20, 40)
            return
        self._step_toward_cell((tx, ty), chamber)

    def _do_idle(self, chamber):
        """Random walk with a gentle pull toward the queen."""
        cx, cy = self.cell()
        q = chamber.queen
        if q is not None and random.random() < 0.3:
            dx = 1 if q.x > cx else (-1 if q.x < cx else 0)
            dy = 1 if q.y > cy else (-1 if q.y < cy else 0)
        else:
            dx = random.choice((-1, 0, 1))
            dy = random.choice((-1, 0, 1))
        # Resolve diagonals to cardinal
        if dx != 0 and dy != 0:
            if random.random() < 0.5:
                dy = 0
            else:
                dx = 0
        if dx == 0 and dy == 0:
            return
        self._set_target_cell(cx + dx, cy + dy, chamber)

    # ================================================================
    #  Task validity
    # ================================================================

    def _target_still_valid(self, chamber):
        if self.state == IDLE:
            return True
        if self.state in (TO_FOOD, TO_HOME, CANNIBALIZE):
            return True
        if self.target is None:
            return False
        if self.state == TEND_QUEEN:
            q = chamber.queen
            return q is not None and q.alive and q.needs_feeding()
        if self.state == TEND_BROOD:
            for b in chamber.brood:
                if ((b.x, b.y) == self.target
                        and b.stage == brood_mod.LARVA
                        and b.alive
                        and b.needs_feeding()):
                    return True
            return False
        return True

    # ================================================================
    #  Cannibalism — last resort under extreme famine
    # ================================================================

    def _do_cannibalize(self, chamber):
        """Walk to target larva and consume it, creating a small
        food pile from recovered energy."""
        tx, ty = self.target
        cx, cy = self.cell()
        if abs(tx - cx) + abs(ty - cy) <= 1:
            for b in list(chamber.brood):
                if ((b.x, b.y) == (tx, ty)
                        and b.stage == brood_mod.LARVA
                        and b.alive):
                    recovered = max(
                        C.BROOD_CANNIBALISM_MIN_PILE,
                        b.fed_total * C.BROOD_CANNIBALISM_RECOVERY,
                    )
                    chamber.brood.remove(b)
                    chamber.colony.food_store += recovered
                    chamber._emit(events.young_died(chamber._tick))
                    break
            self.state  = IDLE
            self.target = None
            self.target_cell = None
            return
        self._step_toward_cell((tx, ty), chamber)

    # ================================================================
    #  Helpers
    # ================================================================

    def _brood_present(self, chamber):
        for b in chamber.brood:
            if b.alive:
                return True
        return False

    def _nearest_hungry_larva(self, chamber):
        cx, cy = self.cell()
        best   = None
        best_d = 1_000_000
        for b in chamber.brood:
            if b.stage != brood_mod.LARVA or not b.alive:
                continue
            if not b.needs_feeding():
                continue
            d = abs(b.x - cx) + abs(b.y - cy)
            if d <= self.sense_radius and d < best_d:
                best   = b
                best_d = d
        return best

    def _least_invested_larva(self, chamber):
        """Return the larva with the lowest fed_total (least wasteful
        to sacrifice for cannibalism)."""
        best = None
        best_fed = 1_000_000.0
        for b in chamber.brood:
            if b.stage != brood_mod.LARVA or not b.alive:
                continue
            if b.fed_total < best_fed:
                best = b
                best_fed = b.fed_total
        return best

    def _detects_food_trail(self, chamber):
        """True if a gatherer recently delivered food to this chamber.
        Uses a delivery signal (not pheromone) so residual to_food
        scent from old deliveries doesn't cause false recruitment."""
        return chamber.food_delivery_signal > 0

    def _within_sense(self, tx, ty):
        cx, cy = self.cell()
        return abs(tx - cx) + abs(ty - cy) <= self.sense_radius

    def _food_pile_adjacent(self, chamber):
        """Return (x, y) of a food pile on or adjacent to the worker."""
        cx, cy = self.cell()
        if (cx, cy) in chamber.food_cells:
            return (cx, cy)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            key = (cx + dx, cy + dy)
            if key in chamber.food_cells and chamber.food_cells[key] > 0:
                return key
        return None

    def _nearest_active_entry(self, chamber, max_dist=None,
                              exclude_face=None):
        """Return the closest active entry point within max_dist,
        or None. Only considers faces with a connected neighbour.
        exclude_face skips that face (e.g. home_face so gatherers
        explore outward, not back toward the nest)."""
        if max_dist is None:
            max_dist = C.ENTRY_ATTRACT_RADIUS
        cx, cy = self.cell()
        best = None
        best_d = max_dist + 1
        for face, pos in C.ENTRY_POINTS.items():
            if chamber.entries.get(face) is None:
                continue
            if face == exclude_face:
                continue
            d = abs(pos[0] - cx) + abs(pos[1] - cy)
            if d < best_d:
                best_d = d
                best   = pos
        return best

    def _explore_or_wander(self, chamber):
        """Exploration with three phases:
        Push (< 5 steps in non-home chamber): walk a few steps inward
        from the entry to clear the handoff cell and prevent instant
        re-crossing. Short enough that the worker explores from the
        entrance rather than being driven to the chamber center.
        Explore (< CHAMBER_EXPLORE_STEPS): random walk, 30% chance
        of steering toward a non-home active entry.
        Exit (>= CHAMBER_EXPLORE_STEPS): deterministic walk to the
        nearest exit — this chamber has nothing, try the next one."""
        if self.chamber_steps >= C.CHAMBER_EXPLORE_STEPS:
            entry = self._nearest_active_entry(chamber, max_dist=200)
            if entry is not None:
                self._step_toward_cell(entry, chamber)
                return
        elif (self.chamber_steps < 5
              and chamber.home_face is not None):
            # Just entered a non-home chamber — push a few steps
            # inward to clear the entry cell (prevents re-crossing).
            # Quantize facing to cardinal for the push direction.
            if abs(self.facing_dx) >= abs(self.facing_dy):
                dx = 1 if self.facing_dx > 0 else -1
                dy = 0
            else:
                dx = 0
                dy = 1 if self.facing_dy > 0 else -1
            cx, cy = self.cell()
            if not self._set_target_cell(cx + dx, cy + dy, chamber):
                self._persistent_forward_step(chamber)
            return
        elif random.random() < 0.3:
            entry = self._nearest_active_entry(
                chamber, exclude_face=chamber.home_face,
            )
            if entry is not None:
                self._step_toward_cell(entry, chamber)
                return
        self._persistent_forward_step(chamber)

    # ---- movement ----

    def _step_toward_cell(self, target, chamber):
        """Walk one cardinal cell toward a target. Prefer the longer
        axis; if blocked, try the other axis."""
        tx, ty = target
        cx, cy = self.cell()
        if (cx, cy) == (tx, ty):
            return
        dx = 1 if tx > cx else (-1 if tx < cx else 0)
        dy = 1 if ty > cy else (-1 if ty < cy else 0)

        if abs(tx - cx) >= abs(ty - cy):
            if not self._set_target_cell(cx + dx, cy, chamber):
                self._set_target_cell(cx, cy + dy, chamber)
        else:
            if not self._set_target_cell(cx, cy + dy, chamber):
                self._set_target_cell(cx + dx, cy, chamber)

    def _persistent_forward_step(self, chamber):
        """Momentum-biased random walk: 70% forward, 12% left, 12%
        right, 6% reverse. Heavily biased so scouts make visible
        progress. The occasional reverse prevents infinite wall-hug."""
        # Quantize facing to cardinal
        if abs(self.facing_dx) >= abs(self.facing_dy):
            fx = 1 if self.facing_dx > 0 else -1
            fy = 0
        else:
            fx = 0
            fy = 1 if self.facing_dy > 0 else -1

        r = random.random()
        if r < 0.70:
            dx, dy = fx, fy
        elif r < 0.82:
            dx, dy = -fy, fx        # left (90° CCW)
        elif r < 0.94:
            dx, dy = fy, -fx        # right (90° CW)
        else:
            dx, dy = -fx, -fy       # reverse

        cx, cy = self.cell()
        if not chamber.in_bounds(cx + dx, cy + dy):
            for adx, ady in ((fx, fy), (-fy, fx), (fy, -fx), (-fx, -fy)):
                if chamber.in_bounds(cx + adx, cy + ady):
                    dx, dy = adx, ady
                    break
        self._set_target_cell(cx + dx, cy + dy, chamber)

    def _emit_interaction(self, chamber, kind, duration_hint):
        """Emit a paired interaction_started + interaction_ended event."""
        bus = chamber._event_bus
        if bus is None:
            return
        pid = bus.next_pair_id()
        bus.emit(events.interaction_started(
            chamber._tick, pid, kind, duration_hint=duration_hint))
        bus.emit(events.interaction_ended(chamber._tick, pid))
