"""Founding queen. Stationary. Lays eggs on a cooldown as long as the
colony has enough food. Pre-nanitic she survives off her internal
reserves (metabolised wing muscle and stored fat); post-nanitic
workers forage and deposit into the colony's abstract food_store.
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
        # Depleted during founding; cannot be accessed by workers.
        self.reserves     = C.FOOD_STORE_START

    # ---- per-tick ----

    def tick(self, chamber):
        if not self.alive:
            return

        # Metabolism — draw from reserves first, then food_store.
        consumed = self._consume(chamber, C.QUEEN_METABOLISM)
        if consumed > 0:
            if self.hunger > 0:
                self.hunger = max(0.0, self.hunger - C.QUEEN_METABOLISM)
        else:
            self.hunger += C.QUEEN_HUNGER_RATE
            if self.hunger >= C.QUEEN_STARVE_THRESHOLD:
                self.alive = False
                return

        # Founding brood care.
        if chamber.colony.population == 0:
            self._tend_founding_brood(chamber)
        elif self.reserves > 0:
            # Workers have hatched — remaining body reserves become
            # communal colony food. The queen now eats from the
            # shared food_store like everyone else.
            chamber.colony.food_store += self.reserves
            self.reserves = 0.0

        # Egg laying — rate and batch size scale with colony state.
        if self.lay_cooldown > 0:
            self.lay_cooldown -= 1
            return

        founding = chamber.colony.population == 0
        if self._can_lay(chamber):
            self._lay(chamber)
            if founding:
                self.lay_cooldown = C.QUEEN_LAY_INTERVAL_FOUNDING
            else:
                pressure = chamber.colony.food_pressure()
                base = C.QUEEN_LAY_INTERVAL_NORMAL
                self.lay_cooldown = base * 2 if pressure > 0.4 else base

    def _consume(self, chamber, amount):
        """Draw from reserves first, then colony food_store."""
        if self.reserves >= amount:
            self.reserves -= amount
            return amount
        from_reserves = self.reserves
        self.reserves = 0.0
        remainder = amount - from_reserves
        store = chamber.colony.food_store
        if store >= remainder:
            chamber.colony.food_store -= remainder
            return amount
        chamber.colony.food_store = max(0.0, store - remainder)
        return from_reserves + store

    def _tend_founding_brood(self, chamber):
        """Feed one hungry under-fed larva per tick."""
        from sim.brood import LARVA
        for b in chamber.brood:
            if b.stage != LARVA or not b.alive:
                continue
            if b.fed_total >= b.food_needed:
                continue
            if not b.needs_feeding():
                continue
            if abs(b.x - self.x) + abs(b.y - self.y) <= 3:
                consumed = self._consume(chamber, C.LARVA_FEED_AMOUNT)
                if consumed > 0:
                    b.feed(consumed)
                return

    # ---- internals ----

    def _can_lay(self, chamber):
        pressure = chamber.colony.food_pressure()
        if pressure > 0.7:
            return False
        if chamber.colony.food_total < C.QUEEN_LAY_FOOD_FLOOR:
            return False
        colony = chamber.colony
        founding = colony.population == 0
        if founding and self.eggs_laid >= C.QUEEN_FOUNDING_EGG_CAP:
            return False
        if not founding:
            bc = colony.brood_counts
            pending = bc.get('egg', 0) + bc.get('larva', 0)
            if pending >= colony.population:
                return False
        return True

    def _lay(self, chamber):
        """Lay a batch of eggs. Batch size scales with worker count."""
        colony = chamber.colony
        if colony.population == 0:
            max_batch = 6
        else:
            max_batch = max(1, min(6, colony.population // 5))
        caste = C.DEFAULT_BROOD_CASTE
        for _ in range(max_batch):
            consumed = self._consume(chamber, C.QUEEN_EGG_FOOD_COST)
            if consumed < C.QUEEN_EGG_FOOD_COST:
                if consumed > 0:
                    self.reserves += consumed
                return
            dx = random.randint(-2, 2)
            dy = random.randint(-2, 2)
            ex = self.x + dx
            ey = self.y + dy
            if chamber.in_bounds(ex, ey) and (dx, dy) != (0, 0):
                chamber.brood.append(Brood(ex, ey, caste=caste))
                self.eggs_laid += 1
            else:
                self.reserves += consumed

    # ---- worker-visible needs ----

    def needs_feeding(self):
        return self.hunger > 0.5
