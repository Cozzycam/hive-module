"""Colony-wide state. One of these exists per colony regardless of how
many Chambers/modules the colony spans. The Coordinator owns it.

Phase 1 has a single chamber so this is trivial, but keeping it separate
means Phase 2 can aggregate brood and population across modules without
changing any chamber code.
"""

import config as C


class Colony:
    def __init__(self):
        # Derived total — sum of all physical food piles + queen
        # reserves across all chambers. Recomputed by Coordinator
        # each tick.
        self.food_store = 0.0
        # Mirror of per-chamber state for quick reads by ants / UI.
        # Updated by Coordinator.tick().
        self.population = 0
        self.forager_count = 0     # workers in TO_FOOD or TO_HOME
        self.brood_counts = {'egg': 0, 'larva': 0, 'pupa': 0}
        # Lifetime counter — used to identify nanitics (first
        # QUEEN_FOUNDING_EGG_CAP workers get shorter lifespans).
        self.total_workers_born = 0

    def food_pressure(self):
        """Returns 0.0 (overstocked) to 1.0 (starving). Drives
        forager deployment and queen egg production.

        The target reserve is FOOD_PRESSURE_TARGET_DAYS of daily
        burn (queen + all workers). Below half the target → pressure
        approaches 1.0; above double the target → pressure is 0.0.
        """
        daily_burn = C.QUEEN_METABOLISM * C.TICKS_PER_SIM_DAY
        daily_burn += (self.population
                       * C.WORKER_METABOLISM_MINOR
                       * C.TICKS_PER_SIM_DAY)
        target = daily_burn * C.FOOD_PRESSURE_TARGET_DAYS
        if target <= 0:
            return 0.5
        ratio = self.food_store / target
        # ratio < 0.3 → pressure ≈ 1.0 (desperate)
        # ratio ~ 1.0 → pressure ≈ 0.5 (moderate)
        # ratio > 2.0 → pressure = 0.0 (overstocked)
        return max(0.0, 1.0 - min(1.0, ratio / 2.0))

    def summary(self):
        return {
            'food':       round(self.food_store, 1),
            'population': self.population,
            'brood':      dict(self.brood_counts),
        }
