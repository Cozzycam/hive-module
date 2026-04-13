"""Founding queen. Stationary. Lays eggs on a cooldown as long as the
colony has enough food. Pre-nanitic she survives off her internal
reserves (metabolised wing muscle and stored fat — no physical pile);
post-nanitic workers must keep food piles stocked by foraging.

All post-founding food interactions are physical. During founding,
the queen draws from self.reserves which is invisible on the grid.
"""

import random

import config as C
from sim.brood import Brood


class Queen:
    def __init__(self, x, y):
        self.x            = x
        self.y            = y
        self.lay_cooldown = C.QUEEN_LAY_INTERVAL  # don't lay on tick 0
        self.eggs_laid    = 0
        self.hunger       = 0.0
        self.alive        = True

        # Internal body reserves — metabolised wing muscle and fat.
        # This is not a physical food pile; it exists inside the queen
        # and cannot be accessed by workers. Depleted during founding,
        # after which the queen depends on worker-deposited food piles.
        self.reserves     = C.FOOD_STORE_START

    # ---- per-tick ----

    def tick(self, chamber):
        if not self.alive:
            return

        # Metabolism — draw from internal reserves first (founding),
        # then from the nearest physical food pile (post-founding).
        if self.reserves >= C.QUEEN_METABOLISM:
            self.reserves -= C.QUEEN_METABOLISM
            if self.hunger > 0:
                self.hunger = max(0.0, self.hunger - C.QUEEN_METABOLISM)
        else:
            consumed = chamber.consume_food(
                self.x, self.y, C.QUEEN_METABOLISM,
            )
            if consumed > 0:
                if self.hunger > 0:
                    self.hunger = max(0.0, self.hunger - C.QUEEN_METABOLISM)
            else:
                self.hunger += C.QUEEN_HUNGER_RATE
                if self.hunger >= C.QUEEN_STARVE_THRESHOLD:
                    self.alive = False
                    return

        # Founding brood care: with no workers around, the queen herself
        # tends her first batch of larvae (real Lasius niger queens do
        # this before nanitics emerge).
        if chamber.colony.population == 0:
            self._tend_founding_brood(chamber)

        # Egg laying
        if self.lay_cooldown > 0:
            self.lay_cooldown -= 1
            return

        if self._can_lay(chamber):
            self._lay(chamber)
            self.lay_cooldown = C.QUEEN_LAY_INTERVAL

    def _consume(self, chamber, amount):
        """Draw from reserves first, then physical piles. Returns
        the actual amount consumed (may be less than requested)."""
        if self.reserves >= amount:
            self.reserves -= amount
            return amount
        # Reserves depleted or insufficient — use what's left,
        # then try physical piles for the remainder.
        from_reserves = self.reserves
        self.reserves = 0.0
        remainder = amount - from_reserves
        from_pile = chamber.consume_food(self.x, self.y, remainder)
        return from_reserves + from_pile

    def _tend_founding_brood(self, chamber):
        """Feed one hungry under-fed larva per tick from the queen's
        internal reserves. Skip larvae that have already met their
        pupation food threshold."""
        from sim.brood import LARVA
        for b in chamber.brood:
            if b.stage != LARVA or not b.alive:
                continue
            if b.fed_total >= b.food_needed:
                continue  # fully grown — waiting on age gate to pupate
            if not b.needs_feeding():
                continue
            # small queen radius — she can only reach brood next to her
            if abs(b.x - self.x) + abs(b.y - self.y) <= 3:
                consumed = self._consume(chamber, C.LARVA_FEED_AMOUNT)
                if consumed > 0:
                    b.feed(consumed)
                return

    # ---- internals ----

    def _can_lay(self, chamber):
        available = self.reserves + chamber.colony.food_store
        if available < C.QUEEN_LAY_FOOD_FLOOR:
            return False
        # Soft cap during founding phase until the first nanitic emerges.
        founding = chamber.colony.population == 0
        if founding and self.eggs_laid >= C.QUEEN_FOUNDING_EGG_CAP:
            return False
        return True

    def _lay(self, chamber):
        consumed = self._consume(chamber, C.QUEEN_EGG_FOOD_COST)
        if consumed < C.QUEEN_EGG_FOOD_COST:
            # Not enough — put back partial amount.
            if consumed > 0:
                self.reserves += consumed
            return
        # Phase 1 founding: only minors. Phase 2 will promote some
        # larvae to majors once the colony passes a population gate.
        caste = C.DEFAULT_BROOD_CASTE
        # Eggs cluster in adjacent cells around the queen.
        for _ in range(6):
            dx = random.randint(-2, 2)
            dy = random.randint(-2, 2)
            ex = self.x + dx
            ey = self.y + dy
            if chamber.in_bounds(ex, ey) and (dx, dy) != (0, 0):
                chamber.brood.append(Brood(ex, ey, caste=caste))
                self.eggs_laid += 1
                return

    # ---- worker-visible needs ----

    def needs_feeding(self):
        return self.hunger > 0.5
