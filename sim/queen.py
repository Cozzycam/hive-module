"""Founding queen. Stationary. Lays eggs on a cooldown as long as the
colony food store is above a floor. Pre-nanitic she survives off the
shared food store (which starts topped up from her wing-muscle reserves);
post-nanitic workers must keep that store filled by trophallaxis or
(Phase 2) foraging.
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

    # ---- per-tick ----

    def tick(self, chamber):
        if not self.alive:
            return

        # Metabolism — drawn from the shared food store. This represents
        # her continuous consumption. If the store is empty she starts
        # accruing hunger and will eventually starve.
        if chamber.colony.food_store >= C.QUEEN_METABOLISM:
            chamber.colony.food_store -= C.QUEEN_METABOLISM
            # Any feeding by workers on a recent tick has already topped
            # up the store, so reduce latent hunger a touch.
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

    def _tend_founding_brood(self, chamber):
        """Feed one hungry under-fed larva per tick from the shared
        store. Skip larvae that have already met their pupation food
        threshold — they're done growing and any extra feeding is
        wasted reserve."""
        from sim.brood import LARVA
        if chamber.colony.food_store < C.LARVA_FEED_AMOUNT:
            return
        for b in chamber.brood:
            if b.stage != LARVA or not b.alive:
                continue
            if b.fed_total >= b.food_needed:
                continue  # fully grown — waiting on age gate to pupate
            if not b.needs_feeding():
                continue
            # small queen radius — she can only reach brood next to her
            if abs(b.x - self.x) + abs(b.y - self.y) <= 3:
                chamber.colony.food_store -= C.LARVA_FEED_AMOUNT
                b.feed(C.LARVA_FEED_AMOUNT)
                return

    # ---- internals ----

    def _can_lay(self, chamber):
        if chamber.colony.food_store < C.QUEEN_LAY_FOOD_FLOOR:
            return False
        # Soft cap during founding phase until the first nanitic emerges.
        founding = chamber.colony.population == 0
        if founding and self.eggs_laid >= C.QUEEN_FOUNDING_EGG_CAP:
            return False
        return True

    def _lay(self, chamber):
        chamber.colony.food_store -= C.QUEEN_EGG_FOOD_COST
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
