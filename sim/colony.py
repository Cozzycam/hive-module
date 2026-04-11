"""Colony-wide state. One of these exists per colony regardless of how
many Chambers/modules the colony spans. The Coordinator owns it.

Phase 1 has a single chamber so this is trivial, but keeping it separate
means Phase 2 can aggregate brood and population across modules without
changing any chamber code.
"""

import config as C


class Colony:
    def __init__(self):
        self.food_store = C.FOOD_STORE_START
        # Mirror of per-chamber state for quick reads by ants / UI.
        # Updated by Coordinator.tick().
        self.population = 0
        self.brood_counts = {'egg': 0, 'larva': 0, 'pupa': 0}

    def summary(self):
        return {
            'food':       round(self.food_store, 1),
            'population': self.population,
            'brood':      dict(self.brood_counts),
        }
