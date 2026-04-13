"""Worker ant — JohnBuffer-inspired marker-following foraging.

States:
  IDLE       — no current task. Random walk near queen, picks up
               domestic duties or goes scouting.
  TEND_QUEEN — walking to the queen to feed her (in-chamber only).
  TEND_BROOD — walking to a hungry larva to feed it (in-chamber only).
  TO_FOOD    — searching for food. Deposits to_home markers whose
               intensity decays with distance from colony. Follows
               to_food gradient laid by returning ants. On finding
               food, flips 180° and switches to TO_HOME.
  TO_HOME    — carrying food back. Deposits to_food markers whose
               intensity decays with distance from food. Follows
               to_home gradient laid by outbound ants. On reaching
               the queen, drops cargo and switches to IDLE.

Movement engine:
  Ants read the 4 cardinal-neighbour marker values directly and step
  toward the strongest. This is the discrete-grid equivalent of
  JohnBuffer's continuous-space random-cone sampling — cheaper,
  deterministic, and far more reliable because we can't miss a
  trail cell the ant is literally adjacent to. If every neighbour
  reads zero the ant falls through to a momentum-biased random
  walk (see _persistent_forward_step) so lost ants still explore.

  Trails are diffused at deposit time (Chamber.deposit_home/food
  writes the primary cell plus its 4 cardinal neighbours at half
  intensity), so a 1-cell walking path becomes a 3-cell-wide trail
  and an ant drifting slightly off the centre line still picks it
  up on the next step.

  Distance from source is encoded in deposit intensity via
  BASE_MARKER_INTENSITY * exp(-MARKER_STEP_DECAY * steps_walked).
  Cells near the colony have strong to_home markers; cells near food
  have strong to_food markers. Gradients point the right way by
  construction — no pathfinding, no corridor sampling, no BFS.
"""

import random
import math

import config as C
from sim import brood as brood_mod


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


class Ant:
    __slots__ = (
        'x', 'y', 'prev_x', 'prev_y', 'state', 'target',
        'move_cooldown', 'age', 'alive',
        'caste', 'move_ticks', 'sense_radius', 'carry_amount',
        'facing_dx', 'facing_dy', 'food_carried',
        'last_dx', 'last_dy',
        'steps_walked', 'ticks_away', 'stall_ticks',
        'idle_cooldown', 'chamber_steps',
        'hunger', 'max_age', 'metabolism',
    )

    def __init__(self, x, y, caste=None, is_nanitic=False):
        self.x             = x
        self.y             = y
        self.prev_x        = x
        self.prev_y        = y
        self.state         = IDLE
        self.target        = None
        self.move_cooldown = 0
        self.age           = 0
        self.alive         = True

        # Random initial facing so ants scatter naturally on spawn.
        self.facing_dx     = random.choice((-1, 1))
        self.facing_dy     = 0
        self.last_dx       = self.facing_dx
        self.last_dy       = 0

        self.food_carried  = 0.0

        # Steps since last source (colony or food). Used to compute
        # deposit intensity: markers weaken with distance from source.
        self.steps_walked  = 0

        # Ticks spent outside the queen chamber. When this exceeds
        # RETURN_HOME_TICKS the ant is forced into TO_HOME so it
        # doesn't get permanently stuck in a non-queen module.
        self.ticks_away    = 0

        # Consecutive movement ticks where the ant didn't change
        # position (stalled at the peak of an exhausted-food
        # gradient). After STALL_THRESHOLD_TICKS the ant ignores
        # the gradient and explores randomly.
        self.stall_ticks   = 0

        # Cooldown before an IDLE ant re-rolls the scouting
        # decision. Prevents the wave-departure problem where
        # every ant re-rolls every tick and all leave at once.
        self.idle_cooldown = 0

        # Steps taken in the CURRENT chamber. Resets on chamber
        # crossing so the ant explores each new chamber before
        # seeking exits. Separate from steps_walked which drives
        # pheromone deposit intensity.
        self.chamber_steps = 0

        # Hunger — accumulates when food_store can't cover
        # metabolism. Worker dies at WORKER_STARVE_THRESHOLD.
        self.hunger        = 0.0

        # Caste-dependent params — baked at spawn time.
        self.caste = caste if caste is not None else C.CASTE_MINOR
        params = C.CASTE_PARAMS[self.caste]
        self.move_ticks    = params['move_ticks']
        self.sense_radius  = params['sense_radius']
        self.carry_amount  = params['carry_amount']
        self.metabolism    = params['metabolism']

        # Lifespan — nanitics (first founding brood) are
        # shorter-lived than regular workers of the same caste.
        if is_nanitic:
            lo, hi = C.WORKER_LIFESPAN_NANITIC
        else:
            lo, hi = params['lifespan']
        self.max_age = random.randint(lo, hi)

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
                return

        # Track time away from the queen chamber.
        if chamber.queen is not None:
            self.ticks_away = 0
        else:
            self.ticks_away += 1

        # Return-home timer — after too long in a non-queen chamber,
        # interrupt whatever the ant is doing and head home.
        if (self.ticks_away >= C.RETURN_HOME_TICKS
                and self.state != TO_HOME):
            self.state        = TO_HOME
            self.target       = None
            self.steps_walked = 0
            self.facing_dx    = -self.facing_dx
            self.facing_dy    = -self.facing_dy

        # IDLE ants only reconsider after their cooldown expires,
        # so departures stagger naturally instead of wave-bursting.
        if self.state == IDLE:
            if self.idle_cooldown > 0:
                self.idle_cooldown -= 1
            else:
                self._pick_task(chamber)
        elif not self._target_still_valid(chamber):
            self._pick_task(chamber)

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
        """Priority: cargo delivery > queen (famine) > domestic > forage."""

        # Food in crop must be delivered first.
        if self.food_carried > 0:
            self.state  = TO_HOME
            self.target = None
            return

        # Non-queen chamber — head home.
        if chamber.queen is None:
            self.state        = TO_HOME
            self.target       = None
            self.steps_walked = 0
            return

        colony   = chamber.colony
        pressure = colony.food_pressure()
        queen    = chamber.queen
        has_food = colony.food_store >= self.carry_amount

        # Stage 2 override — under severe famine, workers feed the
        # queen first if her hunger is critical, before anything else.
        if (pressure > C.FAMINE_SLOWDOWN_PRESSURE
                and queen is not None
                and queen.hunger > C.QUEEN_PRIORITY_HUNGER
                and has_food
                and self._within_sense(queen.x, queen.y)):
            self.state  = TEND_QUEEN
            self.target = (queen.x, queen.y)
            return

        # Domestic: feed queen (normal priority).
        if (queen is not None
                and queen.needs_feeding()
                and has_food
                and self._within_sense(queen.x, queen.y)):
            self.state  = TEND_QUEEN
            self.target = (queen.x, queen.y)
            return

        # Stage 2 — under severe famine, workers stop tending brood
        # (self-preservation). Also trigger brood cannibalism.
        if pressure > C.FAMINE_SLOWDOWN_PRESSURE:
            # Brood cannibalism: convert doomed larvae back to food.
            if (pressure >= C.BROOD_CANNIBALISM_PRESSURE
                    and colony.food_store < self.carry_amount
                    and chamber.cannibalism_cooldown <= 0):
                victim = self._least_invested_larva(chamber)
                if victim is not None:
                    self.state  = CANNIBALIZE
                    self.target = (victim.x, victim.y)
                    chamber.cannibalism_cooldown = C.BROOD_CANNIBALISM_COOLDOWN
                    return
        else:
            # Normal domestic: feed larvae.
            larva = self._nearest_hungry_larva(chamber)
            if larva is not None and has_food:
                self.state  = TEND_BROOD
                self.target = (larva.x, larva.y)
                return

        # Recruitment signal — a returning forager has laid to_food
        # pheromone in the nest. Idle ants that detect the trail
        # follow it immediately, overriding the forager fraction cap.
        # This is the core ant recruitment mechanism.
        if self._detects_food_trail(chamber):
            self.state         = TO_FOOD
            self.target        = None
            self.steps_walked  = 0
            self.chamber_steps = 0
            return

        # Dynamic forager regulation — baseline fraction from
        # pressure, with recovery bounce override.
        target_frac = colony.target_forager_fraction()
        total_pop = colony.population
        if total_pop > 0 and colony.forager_count / total_pop >= target_frac:
            # Enough foragers out — stay home, but reconsider
            # faster under famine so ants don't idle while starving.
            if pressure > C.FAMINE_SLOWDOWN_PRESSURE:
                cd = random.randint(10, 30)
            else:
                cd = random.randint(
                    C.IDLE_RECONSIDER_MIN, C.IDLE_RECONSIDER_MAX,
                )
            self.state         = IDLE
            self.target        = None
            self.idle_cooldown = cd
            return

        # Go forage.
        self.state         = TO_FOOD
        self.target        = None
        self.steps_walked  = 0
        self.chamber_steps = 0

    # ================================================================
    #  Foraging: TO_FOOD (outbound, searching for food)
    # ================================================================

    def _do_to_food(self, chamber):
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        # Scout patience — give up and head home after too long.
        if self.steps_walked > C.SCOUT_PATIENCE_TICKS:
            self.state         = TO_HOME
            self.target        = None
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
                self.steps_walked  = 0
                # Flip facing 180° — "forward" now points home.
                self.facing_dx = -self.facing_dx
                self.facing_dy = -self.facing_dy
                return

        # Stall detection — if the ant hasn't changed position for
        # too many movement ticks it's stuck at the peak of an
        # exhausted-food gradient. Ignore the gradient and explore.
        old_x, old_y = self.x, self.y

        # Movement decision (in priority order):
        # 1. Walk toward a visible food pile within sense radius.
        # 2. Follow to_food gradient (unless stalled — trail is
        #    a dead end, skip straight to random walk).
        # 3. If near an active entry, sometimes steer toward it
        #    so scouts discover passages between chambers.
        # 4. Momentum-biased random walk (exploration).
        visible_pile = chamber.nearest_food_within(
            self.x, self.y, self.sense_radius,
        )
        if visible_pile is not None:
            self._step_toward_cell(visible_pile, chamber)
        elif self.stall_ticks < C.STALL_THRESHOLD_TICKS:
            step = self._sample_markers(chamber, 'food')
            if step is not None:
                dx, dy = step
                self._try_move(dx, dy, chamber)
            else:
                self._explore_or_wander(chamber)
        else:
            self._explore_or_wander(chamber)

        # Update stall counter.
        if self.x == old_x and self.y == old_y:
            self.stall_ticks += 1
        else:
            self.stall_ticks = 0

        # Deposit to_home marker at new position. Intensity decays
        # with steps walked — cells near the colony are strong, far
        # cells are weak. The gradient points home by construction.
        intensity = C.BASE_MARKER_INTENSITY * math.exp(
            -C.MARKER_STEP_DECAY * self.steps_walked
        )
        chamber.deposit_home(self.x, self.y, intensity)
        self.steps_walked += 1
        self.chamber_steps += 1

    # ================================================================
    #  Foraging: TO_HOME (returning with food)
    # ================================================================

    def _do_to_home(self, chamber):
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        if chamber.queen is not None:
            # In the queen chamber — walk to queen, dump food into
            # the colony's abstract food store.
            qx, qy = chamber.queen.x, chamber.queen.y
            if abs(qx - self.x) + abs(qy - self.y) <= 1:
                chamber.colony.food_store += self.food_carried
                self.food_carried  = 0.0
                # Signal nestmates — a forager just delivered food.
                chamber.food_delivery_signal = 200
                self.state         = IDLE
                self.target        = None
                self.steps_walked  = 0
                # Foraging wear — completed trips age the worker.
                self.age += random.randint(*C.FORAGING_TRIP_WEAR)
                self.facing_dx = -self.facing_dx
                self.facing_dy = -self.facing_dy
                return
            self._step_toward_cell((qx, qy), chamber)
        else:
            # Not in the queen chamber — follow to_home gradient.
            step = self._sample_markers(chamber, 'home')
            if step is not None:
                dx, dy = step
                self._try_move(dx, dy, chamber)
            else:
                home_face = chamber.home_face
                if home_face is not None:
                    entry = C.ENTRY_POINTS[home_face]
                    self._step_toward_cell(entry, chamber)
                else:
                    self._persistent_forward_step(chamber)

        # Deposit to_food marker everywhere the forager walks — the
        # trail must extend through the nest so outbound ants can
        # follow it from the queen all the way to the food source.
        if self.food_carried > 0:
            intensity = C.BASE_MARKER_INTENSITY * math.exp(
                -C.MARKER_STEP_DECAY * self.steps_walked
            )
            chamber.deposit_food(self.x, self.y, intensity)
        self.steps_walked += 1

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

        Ties break toward the ant's current facing so committed
        walkers stay committed; remaining ties break randomly so
        ants don't all queue into the same cell.
        """
        read_fn = (chamber.pheromones.home if layer == 'home'
                   else chamber.pheromones.food)
        neighbours = (
            ( 1,  0, read_fn(self.x + 1, self.y)),
            (-1,  0, read_fn(self.x - 1, self.y)),
            ( 0,  1, read_fn(self.x, self.y + 1)),
            ( 0, -1, read_fn(self.x, self.y - 1)),
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

        facing = (self.facing_dx, self.facing_dy)
        if facing in best_picks:
            return facing
        return random.choice(best_picks)

    # ================================================================
    #  Domestic tasks — two-phase: pick up food, then deliver
    # ================================================================

    def _do_tend_brood(self, chamber):
        """Walk to target larva, deduct from food_store, feed it."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        tx, ty = self.target
        if abs(tx - self.x) + abs(ty - self.y) <= 1:
            store = chamber.colony.food_store
            if store >= self.carry_amount:
                for b in chamber.brood:
                    if (b.x, b.y) == (tx, ty) and b.stage == brood_mod.LARVA:
                        chamber.colony.food_store -= self.carry_amount
                        b.feed(self.carry_amount)
                        break
            self.state  = IDLE
            self.target = None
            return
        self._step_toward_cell((tx, ty), chamber)

    def _do_tend_queen(self, chamber):
        """Walk to queen, deduct from food_store, reduce her hunger."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        tx, ty = self.target
        if abs(tx - self.x) + abs(ty - self.y) <= 1:
            store = chamber.colony.food_store
            if store >= self.carry_amount:
                chamber.colony.food_store -= self.carry_amount
                if chamber.queen is not None:
                    chamber.queen.hunger = max(
                        0.0, chamber.queen.hunger - self.carry_amount,
                    )
            self.state  = IDLE
            self.target = None
            return
        self._step_toward_cell((tx, ty), chamber)

    def _do_idle(self, chamber):
        """Random walk with a gentle pull toward the queen."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        q = chamber.queen
        if q is not None and random.random() < 0.3:
            dx = 1 if q.x > self.x else (-1 if q.x < self.x else 0)
            dy = 1 if q.y > self.y else (-1 if q.y < self.y else 0)
        else:
            dx = random.choice((-1, 0, 1))
            dy = random.choice((-1, 0, 1))
        self._try_move(dx, dy, chamber)

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
                        and b.alive):
                    return True
            return False
        return True

    # ================================================================
    #  Cannibalism — last resort under extreme famine
    # ================================================================

    def _do_cannibalize(self, chamber):
        """Walk to target larva and consume it, creating a small
        food pile from recovered energy."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        tx, ty = self.target
        if abs(tx - self.x) + abs(ty - self.y) <= 1:
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
                    break
            self.state  = IDLE
            self.target = None
            return
        self._step_toward_cell((tx, ty), chamber)

    # ================================================================
    #  Helpers
    # ================================================================

    def _move_delay(self, chamber):
        """Movement cooldown, doubled under severe food stress
        (starvation cascade stage 2 — conserve energy). Foragers
        are exempt — they need full speed to find food and save
        the colony."""
        if (chamber.colony.food_pressure() > C.FAMINE_SLOWDOWN_PRESSURE
                and self.state not in (TO_FOOD, TO_HOME)):
            return self.move_ticks * 2
        return self.move_ticks

    def _brood_present(self, chamber):
        for b in chamber.brood:
            if b.alive:
                return True
        return False

    def _nearest_hungry_larva(self, chamber):
        best   = None
        best_d = 1_000_000
        for b in chamber.brood:
            if b.stage != brood_mod.LARVA or not b.alive:
                continue
            if not b.needs_feeding():
                continue
            d = abs(b.x - self.x) + abs(b.y - self.y)
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
        """True if a forager recently delivered food to this chamber.
        Uses a delivery signal (not pheromone) so residual to_food
        scent from old deliveries doesn't cause false recruitment."""
        return chamber.food_delivery_signal > 0

    def _within_sense(self, tx, ty):
        return abs(tx - self.x) + abs(ty - self.y) <= self.sense_radius

    def _food_pile_adjacent(self, chamber):
        """Return (x, y) of a food pile on or adjacent to the ant."""
        if (self.x, self.y) in chamber.food_cells:
            return (self.x, self.y)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            key = (self.x + dx, self.y + dy)
            if key in chamber.food_cells and chamber.food_cells[key] > 0:
                return key
        return None

    def _nearest_active_entry(self, chamber, max_dist=None,
                              exclude_face=None):
        """Return the closest active entry point within max_dist,
        or None. Only considers faces with a connected neighbour.
        exclude_face skips that face (e.g. home_face so foragers
        explore outward, not back toward the nest)."""
        if max_dist is None:
            max_dist = C.ENTRY_ATTRACT_RADIUS
        best = None
        best_d = max_dist + 1
        for face, pos in C.ENTRY_POINTS.items():
            if chamber.entries.get(face) is None:
                continue
            if face == exclude_face:
                continue
            d = abs(pos[0] - self.x) + abs(pos[1] - self.y)
            if d < best_d:
                best_d = d
                best   = pos
        return best

    def _explore_or_wander(self, chamber):
        """Exploration with three phases:
        Push (< 30 steps in non-home chamber): walk straight in the
        facing direction. After handoff, facing points inward, so
        this drives the ant deep into the new chamber.
        Explore (< CHAMBER_EXPLORE_STEPS): random walk, 30% chance
        of steering toward a non-home active entry.
        Exit (>= CHAMBER_EXPLORE_STEPS): deterministic walk to the
        nearest exit — this chamber has nothing, try the next one."""
        if self.chamber_steps >= C.CHAMBER_EXPLORE_STEPS:
            entry = self._nearest_active_entry(chamber, max_dist=200)
            if entry is not None:
                self._step_toward_cell(entry, chamber)
                return
        elif (self.chamber_steps < 30
              and chamber.home_face is not None):
            # Just entered a non-home chamber — push inward using
            # the facing direction set by handoff. Ensures the ant
            # surveys the bulk of the chamber, not just the entrance.
            dx, dy = self.facing_dx, self.facing_dy
            if dx == 0 and dy == 0:
                dx, dy = 1, 0
            if not self._try_move(dx, dy, chamber):
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

    def _step_toward_cell(self, cell, chamber):
        """Walk one cardinal cell toward a target. Prefer the longer
        axis; if blocked, try the other axis."""
        tx, ty = cell
        if (self.x, self.y) == (tx, ty):
            return
        dx = 1 if tx > self.x else (-1 if tx < self.x else 0)
        dy = 1 if ty > self.y else (-1 if ty < self.y else 0)

        if abs(tx - self.x) >= abs(ty - self.y):
            if not self._try_move(dx, 0, chamber):
                self._try_move(0, dy, chamber)
        else:
            if not self._try_move(0, dy, chamber):
                self._try_move(dx, 0, chamber)

    def _persistent_forward_step(self, chamber):
        """Momentum-biased random walk: 70% forward, 12% left, 12%
        right, 6% reverse. Heavily biased so scouts make visible
        progress. The occasional reverse prevents infinite wall-hug."""
        fx, fy = self.facing_dx, self.facing_dy
        if fx == 0 and fy == 0:
            fx, fy = 1, 0
        r = random.random()
        if r < 0.70:
            dx, dy = fx, fy
        elif r < 0.82:
            dx, dy = -fy, fx        # left (90° CCW)
        elif r < 0.94:
            dx, dy = fy, -fx        # right (90° CW)
        else:
            dx, dy = -fx, -fy       # reverse
        if not chamber.in_bounds(self.x + dx, self.y + dy):
            for adx, ady in ((fx, fy), (-fy, fx), (fy, -fx), (-fx, -fy)):
                if chamber.in_bounds(self.x + adx, self.y + ady):
                    dx, dy = adx, ady
                    break
        self._try_move(dx, dy, chamber)

    def _step_toward(self, tx, ty, chamber):
        """Step toward target with move cooldown (domestic tasks)."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self._move_delay(chamber)

        dx = 1 if tx > self.x else (-1 if tx < self.x else 0)
        dy = 1 if ty > self.y else (-1 if ty < self.y else 0)
        if abs(tx - self.x) >= abs(ty - self.y):
            if not self._try_move(dx, 0, chamber):
                self._try_move(0, dy, chamber)
        else:
            if not self._try_move(0, dy, chamber):
                self._try_move(dx, 0, chamber)

    def _try_move(self, dx, dy, chamber):
        """Move one cell. Resolve diagonals to cardinal. Update facing."""
        if dx != 0 and dy != 0:
            if random.random() < 0.5:
                dy = 0
            else:
                dx = 0
        if dx == 0 and dy == 0:
            return False
        nx = self.x + dx
        ny = self.y + dy
        if not chamber.in_bounds(nx, ny):
            return False
        self.x = nx
        self.y = ny
        self.facing_dx = dx
        self.facing_dy = dy
        self.last_dx   = dx
        self.last_dy   = dy
        return True
