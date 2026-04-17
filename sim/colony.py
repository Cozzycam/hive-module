"""Colony-wide state. One of these exists per colony regardless of how
many Chambers/modules the colony spans. The Coordinator owns it.
"""

import config as C


class Colony:
    def __init__(self):
        # Abstract food pool — gatherers deposit into it, metabolism
        # draws from it. Not physical piles; conceptually stored
        # communally in the nest. Queen reserves are separate.
        self.food_store = 0.0
        # Total including queen reserves — set by Coordinator each
        # tick. Used for display and pressure calculations.
        self.food_total = 0.0
        # Mirror of per-chamber state for quick reads by workers / UI.
        # Updated by Coordinator.tick().
        self.population = 0
        self.gatherer_count = 0    # workers in TO_FOOD or TO_HOME
        self.brood_counts = {'egg': 0, 'larva': 0, 'pupa': 0}
        # Lifetime counter — used to identify pioneers (first
        # QUEEN_FOUNDING_EGG_CAP workers get shorter lifespans).
        self.total_workers_born = 0

        # Recovery bounce — after a famine, temporarily boost
        # gathering to restock quickly.
        self.peak_pressure = 0.0
        self.recovery_boost_remaining = 0

    def food_pressure(self):
        """Returns 0.0 (overstocked) to 1.0 (starving). Drives
        gatherer deployment, egg production, and starvation cascade.

        Daily burn uses the ¾-power metabolic scaling so the target
        reserve scales correctly with colony size. Includes projected
        brood feeding costs so the pressure rises proactively when
        larvae are present — not just after the store is already empty.
        """
        scale = C.metabolic_scale_factor(self.population)
        daily_burn = C.QUEEN_METABOLISM * C.TICKS_PER_SIM_DAY
        daily_burn += (self.population * scale
                       * C.WORKER_METABOLISM_MINOR
                       * C.TICKS_PER_SIM_DAY)
        # Brood feeding cost: each larva needs feeding roughly every
        # 500 ticks (hunger threshold 0.5 / rate 0.001). Each feeding
        # costs LARVA_FEED_AMOUNT.
        larva_count = self.brood_counts.get('larva', 0)
        if larva_count > 0:
            feed_interval = 0.5 / C.LARVA_HUNGER_RATE  # 500 ticks
            feeds_per_day = C.TICKS_PER_SIM_DAY / feed_interval
            daily_burn += larva_count * feeds_per_day * C.LARVA_FEED_AMOUNT
        target = daily_burn * C.FOOD_PRESSURE_TARGET_DAYS
        if target <= 0:
            return 0.5
        ratio = self.food_total / target
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

    def target_gatherer_fraction(self):
        """Fraction of workers that should be gathering, accounting
        for recovery bounce override."""
        if self.recovery_boost_remaining > 0:
            return C.MAX_GATHERER_FRACTION
        pressure = self.food_pressure()
        return (C.MIN_GATHERER_FRACTION
                + (C.MAX_GATHERER_FRACTION - C.MIN_GATHERER_FRACTION)
                * pressure)

    def summary(self):
        return {
            'food':       round(self.food_total, 1),
            'population': self.population,
            'brood':      dict(self.brood_counts),
        }
