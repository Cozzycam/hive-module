"""Worker ant. Simple priority-based state machine with pheromone
trail foraging.

States:
  IDLE       — no current task. Will sniff for local needs (queen
               hungry / brood hungry) and otherwise roll a scout check.
  TEND_QUEEN — walking to the queen to feed her (in-chamber only).
  TEND_BROOD — walking to a hungry larva to feed it (in-chamber only).
  OUTBOUND   — empty, heading away from the nest to find food. Drops
               home_scent every step. If it sees food_scent it follows
               the gradient; otherwise it walks toward the nearest
               active entry (scouting through the module network).
  RETURNING  — carrying food, heading back to a queen-bearing chamber.
               Drops food_scent every step. Follows the home_scent
               gradient; when it finds itself in a chamber with a
               queen, it drops the cargo into colony.food_store and
               switches back to IDLE.

Caste-dependent params (move speed, sense radius, carry capacity) are
baked onto the instance at spawn time so later CASTE_PARAMS tuning
doesn't retroactively change existing workers.
"""

import random

import config as C
from sim import brood as brood_mod


# ---- corridor-aware pheromone sampling ----

def _corridor_sample(chamber, cx, cy, dx, dy):
    """Return (food_mag, home_mag) at (cx, cy) for a pheromone
    ray-cast, crossing into a neighbour chamber if the ray exits
    through an active entry cell.

    The ray is cardinal (exactly one of dx/dy is non-zero). If
    (cx, cy) is in-bounds, the local pheromone map is read. If it
    has fallen off the chamber edge, we check whether the exit is
    through an active face and whether the ray's lateral position
    is aligned with that face's entry cell — if so, the ray
    continues into the neighbour and samples its opposite-side
    cell. Otherwise we return None, signalling the ray hit a wall.

    Entry cells are mirrored by Chamber.deposit_home/food, so the
    first cell past the chamber edge corresponds to the cell ONE
    PAST the neighbour's entry cell (not the entry cell itself) —
    skipping it prevents double-counting the mirrored deposit.
    """
    if chamber.in_bounds(cx, cy):
        return (chamber.pheromones.food(cx, cy),
                chamber.pheromones.home(cx, cy))

    # Out of bounds — figure out which face the ray exited through.
    if cx >= chamber.width:
        face = 'E'
    elif cx < 0:
        face = 'W'
    elif cy >= chamber.height:
        face = 'S'
    elif cy < 0:
        face = 'N'
    else:
        return None

    neighbor = chamber.neighbors.get(face)
    if neighbor is None:
        return None

    entry_x, entry_y = C.ENTRY_POINTS[face]
    # Cardinal rays can only thread through the corridor if their
    # lateral coordinate matches the entry cell's.
    if face in ('E', 'W'):
        if cy != entry_y:
            return None
    else:
        if cx != entry_x:
            return None

    # Map into the neighbour's coordinate system, skipping the
    # neighbour's entry cell (which is already mirrored with this
    # chamber's entry cell — reading both would double-count).
    w = chamber.width
    h = chamber.height
    if face == 'E':
        nx = cx - w + 1     # cx == w (k beyond wall) → nx == 1
        ny = cy
    elif face == 'W':
        nx = w + cx - 1     # cx == -1 → nx == w - 2 (inner neighbour cell)
        ny = cy
    elif face == 'S':
        nx = cx
        ny = cy - h + 1
    else:  # N
        nx = cx
        ny = h + cy - 1

    if not neighbor.in_bounds(nx, ny):
        return None
    return (neighbor.pheromones.food(nx, ny),
            neighbor.pheromones.home(nx, ny))


# task / state tags
IDLE        = 'idle'
TEND_QUEEN  = 'tend_queen'
TEND_BROOD  = 'tend_brood'
OUTBOUND    = 'outbound'
RETURNING   = 'returning'

# legacy alias — still referenced in module_panel + old tests
FORAGE      = OUTBOUND


class Ant:
    __slots__ = (
        'x', 'y', 'prev_x', 'prev_y', 'state', 'target',
        'move_cooldown', 'age', 'alive',
        'caste', 'move_ticks', 'sense_radius', 'carry_amount',
        'facing_dx', 'facing_dy', 'food_carried',
        'last_dx', 'last_dy', 'explorer', 'outbound_ticks',
    )

    def __init__(self, x, y, caste=None):
        self.x             = x
        self.y             = y
        self.prev_x        = x        # snapshotted each tick for smooth render
        self.prev_y        = y
        self.state         = IDLE
        self.target        = None     # (x, y) tuple or None
        self.move_cooldown = 0
        self.age           = 0
        self.alive         = True

        # Facing direction — updated on every successful step. Starts
        # pointing east arbitrarily. last_dx/dy remember the cardinal
        # direction of the most recent successful step so the gradient
        # sensor can penalise immediate reversal.
        self.facing_dx     = 1
        self.facing_dy     = 0
        self.last_dx       = 1
        self.last_dy       = 0

        # How much food is in the crop right now.
        self.food_carried  = 0.0

        # Tick counter for the current OUTBOUND trip. Ants that have
        # been scouting unsuccessfully for too long flip to RETURNING
        # empty-handed and head back to the nest — this is what lets
        # the HOME_WORKER_RESERVE balance actually take effect.
        self.outbound_ticks = 0

        # Explorer phenotype — ~10% of ants have it. Explorers ignore
        # the dominant pheromone signal 20% of the time while
        # gradient-stepping, which lets the colony occasionally find
        # shortcuts or rediscover depleted food sources instead of
        # locking in on the first trail it finds.
        self.explorer = random.random() < C.EXPLORER_CHANCE

        # Caste-dependent params
        self.caste = caste if caste is not None else C.CASTE_MINOR
        params = C.CASTE_PARAMS[self.caste]
        self.move_ticks    = params['move_ticks']
        self.sense_radius  = params['sense_radius']
        self.carry_amount  = params['carry_amount']

    # ---- per-tick ----

    def tick(self, chamber):
        if not self.alive:
            return

        self.age += 1

        if self.state == IDLE or not self._target_still_valid(chamber):
            self._pick_task(chamber)

        if self.state == TEND_BROOD:
            self._do_tend_brood(chamber)
        elif self.state == TEND_QUEEN:
            self._do_tend_queen(chamber)
        elif self.state == OUTBOUND:
            self._do_outbound(chamber)
        elif self.state == RETURNING:
            self._do_returning(chamber)
        else:
            self._do_idle(chamber)

    # ---- task selection ----

    def _pick_task(self, chamber):
        """Priority-based: carrying food beats everything (the crop has
        to be dumped before this ant does anything else), then in-chamber
        brood care, then scout."""
        # Cargo delivery has the highest priority. If the ant has food
        # in its crop it MUST walk to the queen and dump it, otherwise
        # it risks getting diverted into tend_queen/tend_brood (which
        # draw from food_store, not from the crop) and carrying the
        # payload around indefinitely. This is the fix for "ants take
        # food to the edge of the module boundary and then go back and
        # forth" — after handoff, food_carried alone decides the state.
        if self.food_carried > 0:
            self.state  = RETURNING
            self.target = None
            return

        queen = chamber.queen
        if (queen is not None
                and queen.needs_feeding()
                and chamber.colony.food_store >= self.carry_amount
                and self._within_sense(queen.x, queen.y)):
            self.state  = TEND_QUEEN
            self.target = (queen.x, queen.y)
            return

        larva = self._nearest_hungry_larva(chamber)
        if (larva is not None
                and chamber.colony.food_store
                    >= self.carry_amount + C.MIN_BROOD_FEED_RESERVE):
            self.state  = TEND_BROOD
            self.target = (larva.x, larva.y)
            return

        # Worker reservation: when we're in a queen chamber with brood
        # present and the queen chamber holds fewer than
        # HOME_WORKER_RESERVE of the whole colony, refuse to scout
        # and stay home to tend brood. Without this, idle workers all
        # eventually drift out and the brood chamber empties.
        if chamber.queen is not None and self._brood_present(chamber):
            total_pop = chamber.colony.population
            here_pop  = len(chamber.workers)
            if total_pop > 0 and here_pop / total_pop < C.HOME_WORKER_RESERVE:
                self.state  = IDLE
                self.target = None
                return

        # Non-queen chamber: no domestic task is possible here (no
        # queen to feed, no brood to tend) so there is no reason to
        # fall through to IDLE. Random-walking an idle ant in a
        # non-queen chamber is actively harmful — it eventually
        # drifts onto the home-face entry cell and sends the ant
        # back to the nest empty-handed, which shows up as "ants
        # piling up at the corridor without making progress". Keep
        # them scouting instead.
        if chamber.queen is None:
            self.state  = OUTBOUND
            self.target = self._random_outbound_entry(chamber)
            self.outbound_ticks = 0
            return

        # Queen chamber: roll the scout dice. Workers in a queen
        # chamber with a visible food_scent are more likely to head
        # out — that's how a fresh trail recruits foragers.
        roll = random.random()
        if self._food_scent_nearby(chamber):
            # Food trail leads out of the nest — recruit aggressively.
            if roll < 0.7:
                entry = self._nearest_active_entry(chamber)
                if entry is not None:
                    self.state  = OUTBOUND
                    self.target = entry
                    self.outbound_ticks = 0
                    return
        if roll < C.SCOUT_PROBABILITY:
            entry = self._nearest_active_entry(chamber)
            if entry is not None:
                self.state  = OUTBOUND
                self.target = entry
                self.outbound_ticks = 0
                return
            # No active exits yet (brand-new colony with no
            # neighbours attached) — just stay home.

        self.state  = IDLE
        self.target = None

    def _brood_present(self, chamber):
        for b in chamber.brood:
            if b.alive:
                return True
        return False

    def _target_still_valid(self, chamber):
        if self.state == IDLE:
            return True
        if self.state == OUTBOUND:
            # Re-pick target if the pointed-at entry became inactive
            # (shouldn't normally happen). Otherwise stay the course.
            return True
        if self.state == RETURNING:
            return True
        if self.target is None:
            return False
        if self.state == TEND_QUEEN:
            q = chamber.queen
            return q is not None and q.alive and q.needs_feeding()
        if self.state == TEND_BROOD:
            for b in chamber.brood:
                if (b.x, b.y) == self.target and b.stage == brood_mod.LARVA and b.alive:
                    return True
            return False
        return True

    def _nearest_hungry_larva(self, chamber):
        best = None
        best_d = 1_000_000
        for b in chamber.brood:
            if b.stage != brood_mod.LARVA or not b.alive:
                continue
            if not b.needs_feeding():
                continue
            d = abs(b.x - self.x) + abs(b.y - self.y)
            if d <= self.sense_radius and d < best_d:
                best = b
                best_d = d
        return best

    def _within_sense(self, tx, ty):
        return abs(tx - self.x) + abs(ty - self.y) <= self.sense_radius

    def _nearest_active_entry(self, chamber):
        best = None
        best_d = 1_000_000
        for face, neighbour in chamber.entries.items():
            if neighbour is None:
                continue
            ex, ey = C.ENTRY_POINTS[face]
            d = abs(ex - self.x) + abs(ey - self.y)
            if d < best_d:
                best = (ex, ey)
                best_d = d
        return best

    def _random_outbound_entry(self, chamber):
        """Pick a random active entry that is NOT the home-ward face.

        Returns a (x, y) cell. Random (rather than nearest) so that
        when a junction chamber has multiple non-home branches the
        scouts distribute across both instead of all deterministically
        funnelling into whichever happens to be closer. Returns None
        if the only active entry IS the home_face (dead-end chamber)
        or there are no active entries at all.
        """
        home = chamber.home_face
        candidates = []
        for face, neighbour in chamber.entries.items():
            if neighbour is None or face == home:
                continue
            candidates.append(C.ENTRY_POINTS[face])
        if not candidates:
            return None
        return random.choice(candidates)

    def _target_is_entry_of(self, chamber, target):
        """True if `target` matches an active entry cell of `chamber`.
        Used by outbound ants to tell if their cached target is still
        meaningful after crossing into a new chamber."""
        if target is None:
            return False
        for face, neighbour in chamber.entries.items():
            if neighbour is None:
                continue
            if C.ENTRY_POINTS[face] == target:
                return True
        return False

    def _food_scent_nearby(self, chamber):
        """Is there a food trail within our detection radius? Used by
        _pick_task to decide whether to recruit this worker out of the
        nest along a newly-laid trail."""
        r = C.FOOD_SCENT_SENSE_RADIUS
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                if abs(dx) + abs(dy) > r:
                    continue
                if chamber.pheromones.food(self.x + dx, self.y + dy) > 0:
                    return True
        return False

    # ---- actions ----

    def _do_tend_brood(self, chamber):
        tx, ty = self.target
        if abs(tx - self.x) + abs(ty - self.y) <= 1:
            if chamber.colony.food_store >= self.carry_amount:
                for b in chamber.brood:
                    if (b.x, b.y) == (tx, ty) and b.stage == brood_mod.LARVA:
                        chamber.colony.food_store -= self.carry_amount
                        b.feed(self.carry_amount)
                        break
            self.state = IDLE
            self.target = None
            return
        self._step_toward(tx, ty, chamber)

    def _do_tend_queen(self, chamber):
        tx, ty = self.target
        if abs(tx - self.x) + abs(ty - self.y) <= 1:
            if chamber.colony.food_store >= self.carry_amount:
                chamber.colony.food_store -= self.carry_amount
                chamber.queen.hunger = max(
                    0.0, chamber.queen.hunger - self.carry_amount
                )
            self.state = IDLE
            self.target = None
            return
        self._step_toward(tx, ty, chamber)

    def _do_outbound(self, chamber):
        """Outbound: grab food if adjacent, step forward, drop home_scent.

        Movement logic depends on which chamber we're in:
          - Queen chamber: walk toward the nearest active entry to
            leave the nest.
          - Non-queen chamber: follow a food_scent gradient if any
            exists, otherwise momentum-biased random walk.

        Scouts stay outbound until they pick up food. The
        HOME_WORKER_RESERVE gate in _pick_task is what keeps the
        bulk of the colony at home — scouts don't need a patience
        timeout on top of that.

        The home_scent deposit happens AT THE END of the tick (after
        the step), not before — this ensures entry cells get
        populated. Ants land on an entry cell via their final step,
        get handed off before their next tick, and so under the old
        "deposit at start of tick" pattern the entry cell itself
        never got a direct deposit. Depositing after the step fixes
        that and makes the corridor visible to Chamber.deposit_home's
        mirror logic.
        """
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self.move_ticks

        # Food pickup — if we're standing on or adjacent to a food
        # pile, grab as much as we can carry and flip to RETURNING.
        # Reverse facing 180° so the forward arc for gradient-following
        # points back the way we came (home_scent is strongest there).
        # This early-return skips the end-of-tick deposit on purpose:
        # the state has changed to RETURNING and home_scent isn't
        # appropriate any more.
        pile = self._food_pile_adjacent(chamber)
        if pile is not None:
            px, py = pile
            taken = chamber.take_food(px, py, C.FORAGE_FOOD_PICKUP)
            if taken > 0:
                self.food_carried = taken
                self.state  = RETURNING
                self.target = None
                self.facing_dx = -self.facing_dx
                self.facing_dy = -self.facing_dy
                return

        # Step logic — picks a direction and calls _try_move or
        # _step_toward_cell. After this block runs (or falls
        # through), the trailing deposit fires at the new position.
        if chamber.queen is not None:
            # In the nest — walk toward an exit. Jittered so the
            # stream of outbound ants doesn't file along a single
            # ruler-straight corridor to the exit cell.
            entry = self._nearest_active_entry(chamber)
            if entry is None:
                self._random_step(chamber)
            else:
                self._step_toward_cell(entry, chamber, jitter=C.PATH_JITTER)
        else:
            # In an outworld-like chamber — first check if there's a
            # food pile within our sense radius. If yes, walk straight
            # to it; this is a lot more reliable than relying purely on
            # food_scent gradient detection, which only recruits ants
            # to *existing* known trails.
            visible_pile = chamber.nearest_food_within(
                self.x, self.y, self.sense_radius
            )
            if visible_pile is not None:
                self._step_toward_cell(visible_pile, chamber,
                                       jitter=C.PATH_JITTER)
            else:
                # Try to follow a food_scent trail. Explorer ants
                # occasionally ignore the gradient even when it's
                # strong, which is how the colony finds shortcuts and
                # rediscovers depleted sources.
                step = self._step_by_score(chamber, food_weight=3.0,
                                           home_weight=0.0)
                if step is not None:
                    if self.explorer and random.random() < C.EXPLORER_DEVIATE:
                        self._persistent_forward_step(chamber)
                    else:
                        dx, dy = step
                        self._try_move(dx, dy, chamber)
                else:
                    # No food_scent signal. Push deeper into the
                    # network by walking toward a cached non-home
                    # exit target. The target is chosen randomly on
                    # first entry to each chamber so a junction
                    # distributes scouts across both branches instead
                    # of funnelling everyone into the nearest one.
                    # Handoff resets self.target so _do_outbound picks
                    # fresh on arrival in a new chamber.
                    if not self._target_is_entry_of(chamber, self.target):
                        self.target = self._random_outbound_entry(chamber)
                    if self.target is not None:
                        self._step_toward_cell(self.target, chamber,
                                               jitter=C.PATH_JITTER)
                    else:
                        # Dead-end chamber — no non-home exit. Fall
                        # back to momentum walking.
                        self._persistent_forward_step(chamber)

        # Deposit home trail at the new position. Routed through the
        # chamber wrapper so deposits on entry cells mirror to the
        # neighbour's opposite entry cell, keeping both sides of a
        # corridor in sync. Pass the ant's last cardinal step (which
        # is the step just taken) as the flow-direction hint.
        chamber.deposit_home(
            self.x, self.y, C.PHEROMONE_DEPOSIT_OUT,
            self.last_dx, self.last_dy,
        )

    def _do_returning(self, chamber):
        """Returning: walk back toward the queen / home_face entry,
        then drop food_scent at the new cell.

        In single-hop topology (Phase 2.x) the nearest active entry
        from a non-queen chamber always leads home, so deterministic
        entry-walking is reliable. Pheromone-gradient home-finding
        for multi-hop networks comes back in Phase 3.

        Like _do_outbound, the food_scent deposit happens AT THE END
        of the tick (after the step). This is what makes entry cells
        show up in the pheromone field — under the old "deposit at
        start of tick" pattern the ant's final step onto the entry
        cell was never captured because the handoff fired before the
        next tick could deposit.
        """
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self.move_ticks

        # Home! Walk to the queen and transfer the cargo by
        # trophallaxis once adjacent. Radius 1 now that collisions
        # don't block the approach — returners can stack right on
        # top of her for a visible pile-up during busy foraging.
        # This early-return skips the trailing deposit because the
        # state has just changed to IDLE and there's no cargo left
        # to lay down a trail for.
        if chamber.queen is not None:
            qx, qy = chamber.queen.x, chamber.queen.y
            if abs(qx - self.x) + abs(qy - self.y) <= 1:
                chamber.colony.food_store += self.food_carried
                self.food_carried = 0.0
                self.state  = IDLE
                self.target = None
                return
            self._step_toward_cell((qx, qy), chamber,
                                   jitter=C.PATH_JITTER)
        else:
            # Non-queen chamber — use the precomputed home_face (set
            # by Coordinator BFS from the founding chamber). This is
            # the face whose entry leads along the shortest path back
            # to the nest. Walking toward the NEAREST active entry
            # would oscillate at multi-hop junctions because the entry
            # an ant just crossed through is the closest one.
            home_face = chamber.home_face
            if home_face is not None:
                entry = C.ENTRY_POINTS[home_face]
                self._step_toward_cell(entry, chamber, jitter=C.PATH_JITTER)
            else:
                # Fallback — chamber has no home_face (isolated?).
                # Use nearest active entry as a best-effort.
                entry = self._nearest_active_entry(chamber)
                if entry is None:
                    self._random_step(chamber)
                else:
                    self._step_toward_cell(entry, chamber,
                                           jitter=C.PATH_JITTER)

        # Deposit food trail at the new position. Routed through the
        # chamber wrapper so entry-cell deposits mirror to the
        # neighbour side of the corridor, keeping the trail
        # contiguous across module boundaries. Only deposit when
        # actually carrying food — empty returners (not reachable
        # under current logic but defensive) don't lay trails that
        # lead nowhere.
        if self.food_carried > 0:
            chamber.deposit_food(
                self.x, self.y, C.PHEROMONE_DEPOSIT_RETURN,
                self.last_dx, self.last_dy,
            )

    def _do_idle(self, chamber):
        """Random walk with a gentle pull toward the queen so idle
        workers cluster near her (they're naturally allowed to pile
        up on the brood area now that there's no collision check)."""
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self.move_ticks

        q = chamber.queen
        if q is not None and random.random() < 0.3:
            dx = 1 if q.x > self.x else (-1 if q.x < self.x else 0)
            dy = 1 if q.y > self.y else (-1 if q.y < self.y else 0)
        else:
            dx = random.choice((-1, 0, 1))
            dy = random.choice((-1, 0, 1))
        self._try_move(dx, dy, chamber)

    # ---- pheromone gradient stepping ----

    # Distance falloff weights for the ray-cast in _step_by_score.
    # Closer cells dominate but distant cells still contribute, so
    # a trail is detectable from several steps away. Widened from 3
    # to 5 cells so ants pick up trails earlier and are less likely
    # to walk right past them.
    _RAY_WEIGHTS = (1.0, 0.75, 0.5, 0.3, 0.15)

    def _step_by_score(self, chamber, food_weight, home_weight,
                       min_signal=0.15):
        """Pick a cardinal step that maximises

            score = food_weight * food_ray(n)
                  + home_weight * home_ray(n)
                  + noise
                  - reverse_penalty

        For each cardinal direction the sensor sums the pheromone
        values in the first ~5 cells along that ray, weighted by
        distance. This lets ants detect a trail from a meaningful
        distance instead of only when standing next to it. Heavy
        reverse penalty stops immediate back-stepping.

        After picking the best direction, a PHEROMONE_JITTER chance
        replaces it with a random perpendicular step so the path
        meanders naturally instead of looking like a ruler-straight
        line down the trail.

        Returns (dx, dy), or None if no direction carries a
        meaningful pheromone signal.
        """
        rev_dx = -self.last_dx
        rev_dy = -self.last_dy

        best_score      = None
        best_raw_scent  = 0.0          # signal strength before noise/penalty
        best_dx         = 0
        best_dy         = 0
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            # First cell must be in-bounds, otherwise the step isn't
            # even physically possible.
            if not chamber.in_bounds(self.x + dx, self.y + dy):
                continue
            food_sum = 0.0
            home_sum = 0.0
            for k, weight in enumerate(self._RAY_WEIGHTS, start=1):
                sample = _corridor_sample(
                    chamber, self.x + dx * k, self.y + dy * k, dx, dy,
                )
                if sample is None:
                    break
                fv, hv = sample
                food_sum += weight * fv
                home_sum += weight * hv
            raw   = food_weight * food_sum + home_weight * home_sum
            score = raw + random.random() * C.PHEROMONE_SENSE_NOISE
            if (dx, dy) == (rev_dx, rev_dy):
                score -= 1.0
            if best_score is None or score > best_score:
                best_score     = score
                best_raw_scent = raw
                best_dx        = dx
                best_dy        = dy

        if best_score is None:
            return None
        if best_raw_scent < min_signal:
            return None

        # Natural-meander jitter — sometimes step perpendicular to
        # the gradient direction instead of strictly along it. Keeps
        # trail-followers from drawing ruler-straight lines.
        if random.random() < C.PHEROMONE_JITTER:
            if best_dx != 0:
                pdy = random.choice((-1, 1))
                if chamber.in_bounds(self.x, self.y + pdy):
                    return (0, pdy)
            elif best_dy != 0:
                pdx = random.choice((-1, 1))
                if chamber.in_bounds(self.x + pdx, self.y):
                    return (pdx, 0)

        return (best_dx, best_dy)

    def _step_toward_cell(self, cell, chamber, jitter=0.0):
        """Walk one cell toward a target.

        With jitter=0 this is strictly axis-preferred cardinal stepping
        toward the target — used for short approaches where we don't
        want meander (tend_queen, tend_brood).

        With jitter>0, each step has that probability of being replaced
        with a random perpendicular step instead — used for long-distance
        walks (walking to an exit, walking across a chamber to the
        queen) so the path wobbles naturally instead of being a
        ruler-straight line. The primary-axis corrective walk will
        pull the ant back to the target direction on subsequent ticks
        so the overall path still converges on the destination.
        """
        tx, ty = cell
        if (self.x, self.y) == (tx, ty):
            return
        dx = 1 if tx > self.x else (-1 if tx < self.x else 0)
        dy = 1 if ty > self.y else (-1 if ty < self.y else 0)

        if jitter > 0.0 and random.random() < jitter:
            # Random perpendicular jitter relative to the primary
            # (longer) axis. If the perpendicular step would land
            # off-grid, fall through to the normal axis walk below.
            if abs(tx - self.x) >= abs(ty - self.y):
                pdy = random.choice((-1, 1))
                if chamber.in_bounds(self.x, self.y + pdy):
                    self._try_move(0, pdy, chamber)
                    return
            else:
                pdx = random.choice((-1, 1))
                if chamber.in_bounds(self.x + pdx, self.y):
                    self._try_move(pdx, 0, chamber)
                    return

        if abs(tx - self.x) >= abs(ty - self.y):
            if not self._try_move(dx, 0, chamber):
                self._try_move(0, dy, chamber)
        else:
            if not self._try_move(0, dy, chamber):
                self._try_move(dx, 0, chamber)

    def _persistent_forward_step(self, chamber):
        """Momentum-biased random walk: 72% forward, 12% left, 12%
        right, 4% reverse. Heavily biased toward forward so scouts
        make visible progress instead of meandering in place — earlier
        tuning was much squigglier (45/25/25/5) and made exploration
        feel jittery. The occasional reverse break still prevents
        infinite wall-hugging. If the chosen direction is off-grid or
        blocked, fall through to any in-bounds cardinal.
        """
        fx, fy = self.facing_dx, self.facing_dy
        if fx == 0 and fy == 0:
            fx, fy = 1, 0
        r = random.random()
        if r < 0.72:
            dx, dy = fx, fy                  # forward
        elif r < 0.84:
            dx, dy = -fy, fx                 # left (90° CCW)
        elif r < 0.96:
            dx, dy = fy, -fx                 # right (90° CW)
        else:
            dx, dy = -fx, -fy                # reverse
        if not chamber.in_bounds(self.x + dx, self.y + dy):
            for adx, ady in ((fx, fy), (-fy, fx), (fy, -fx), (-fx, -fy)):
                if chamber.in_bounds(self.x + adx, self.y + ady):
                    dx, dy = adx, ady
                    break
        self._try_move(dx, dy, chamber)

    def _food_pile_adjacent(self, chamber):
        if (self.x, self.y) in chamber.food_cells:
            return (self.x, self.y)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            key = (self.x + dx, self.y + dy)
            if key in chamber.food_cells and chamber.food_cells[key] > 0:
                return key
        return None

    # ---- movement helpers ----

    def _step_toward(self, tx, ty, chamber):
        if self.move_cooldown > 0:
            self.move_cooldown -= 1
            return
        self.move_cooldown = self.move_ticks

        dx = 1 if tx > self.x else (-1 if tx < self.x else 0)
        dy = 1 if ty > self.y else (-1 if ty < self.y else 0)
        if abs(tx - self.x) >= abs(ty - self.y):
            if not self._try_move(dx, 0, chamber):
                self._try_move(0, dy, chamber)
        else:
            if not self._try_move(0, dy, chamber):
                self._try_move(dx, 0, chamber)

    def _random_step(self, chamber):
        dx = random.choice((-1, 0, 1))
        dy = random.choice((-1, 0, 1))
        self._try_move(dx, dy, chamber)

    def _try_move(self, dx, dy, chamber):
        # Clamp to 4-connectivity: if the caller asked for a diagonal
        # step, drop one axis at random. Keeps facing strictly cardinal.
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
        # No worker-worker collision — real ants pile up on each
        # other all the time, especially around the queen, on trails,
        # and at nest entrances. Letting them share cells removes a
        # whole class of traffic-jam deadlocks and is more biological.
        self.x = nx
        self.y = ny
        # Facing and last-step become the cardinal step we just took.
        self.facing_dx = dx
        self.facing_dy = dy
        self.last_dx   = dx
        self.last_dy   = dy
        return True
