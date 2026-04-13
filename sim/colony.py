"""Colony-wide state. One of these exists per colony regardless of how
many Chambers/modules the colony spans. The Coordinator owns it.
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

        # Recovery bounce — after a famine, temporarily boost
        # foraging to restock quickly.
        self.peak_pressure = 0.0
        self.recovery_boost_remaining = 0

    def food_pressure(self):
        """Returns 0.0 (overstocked) to 1.0 (starving). Drives
        forager deployment, egg production, and starvation cascade.

        Daily burn uses the ¾-power metabolic scaling so the target
        reserve scales correctly with colony size.
        """
        scale = C.metabolic_scale_factor(self.population)
        daily_burn = C.QUEEN_METABOLISM * C.TICKS_PER_SIM_DAY
        daily_burn += (self.population * scale
                       * C.WORKER_METABOLISM_MINOR
                       * C.TICKS_PER_SIM_DAY)
        target = daily_burn * C.FOOD_PRESSURE_TARGET_DAYS
        if target <= 0:
            return 0.5
        ratio = self.food_store / target
        return max(0.0, 1.0 - min(1.0, ratio / 2.0))

    def update_recovery_boost(self):
        """Called by Coordinator each tick after pressure is computed.
        Tracks peak pressure and triggers the recovery bounce when
        pressure drops below 0.5 after exceeding the threshold."""
        pressure = self.food_pressure()
        if pressure > self.peak_pressure:
            self.peak_pressure = pressure

        if self.recovery_boost_remaining > 0:
            self.recovery_boost_remaining -= 1
        elif (self.peak_pressure >= C.RECOVERY_BOOST_THRESHOLD
              and pressure < 0.5):
            # Famine relieved — trigger boost.
            self.recovery_boost_remaining = C.RECOVERY_BOOST_DURATION
            self.peak_pressure = pressure  # reset so it doesn't re-trigger

    def target_forager_fraction(self):
        """Fraction of workers that should be foraging, accounting
        for recovery bounce override."""
        if self.recovery_boost_remaining > 0:
            return C.MAX_FORAGER_FRACTION
        pressure = self.food_pressure()
        return (C.MIN_FORAGER_FRACTION
                + (C.MAX_FORAGER_FRACTION - C.MIN_FORAGER_FRACTION)
                * pressure)

    def summary(self):
        return {
            'food':       round(self.food_store, 1),
            'population': self.population,
            'brood':      dict(self.brood_counts),
        }
