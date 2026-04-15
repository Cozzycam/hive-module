"""Brood lifecycle: egg -> larva -> pupa -> nanitic (worker).

Each Brood object carries its destined caste (minor/major) from egg
onward. Majors require more food and more time as larvae to grow their
characteristic oversized heads. The queen sets the destined caste at
laying; Phase 1 founding produces only minors.

When a pupa completes, chamber.tick() consumes the brood and spawns an
Ant of the matching caste in its place.
"""

import config as C


EGG   = 'egg'
LARVA = 'larva'
PUPA  = 'pupa'
DEAD  = 'dead'


class Brood:
    __slots__ = (
        'x', 'y', 'stage', 'age', 'hunger', 'fed_total',
        'caste', 'larva_duration', 'food_needed',
    )

    def __init__(self, x, y, caste=None):
        self.x         = x
        self.y         = y
        self.stage     = EGG
        self.age       = 0        # ticks spent in current stage
        self.hunger    = 0.0      # larva only
        self.fed_total = 0.0      # cumulative feeding received as a larva

        # Caste destiny (minor by default during founding)
        self.caste = caste if caste is not None else C.DEFAULT_BROOD_CASTE
        params = C.CASTE_PARAMS[self.caste]
        self.larva_duration = params['larva_duration']
        self.food_needed    = params['larva_food_needed']

    # ---- lifecycle ----

    def tick(self):
        """Advance one tick. Returns 'hatch' if a pupa is ready to become
        a nanitic (chamber consumes the brood in that case). Otherwise
        returns None."""
        if self.stage == DEAD:
            return None

        self.age += 1

        if self.stage == EGG:
            if self.age >= C.EGG_DURATION:
                self.stage = LARVA
                self.age   = 0
            return None

        if self.stage == LARVA:
            self.hunger += C.LARVA_HUNGER_RATE
            if self.hunger >= C.LARVA_STARVE:
                self.stage = DEAD
                return None
            if (self.age >= self.larva_duration
                    and self.fed_total >= self.food_needed):
                self.stage = PUPA
                self.age   = 0
            return None

        if self.stage == PUPA:
            if self.age >= C.PUPA_DURATION:
                return 'hatch'
            return None

        return None

    # ---- feeding ----

    def needs_feeding(self):
        """True if this brood is a larva and would benefit from being fed."""
        return self.stage == LARVA and self.hunger > 0.5

    def feed(self, amount):
        """Worker drops food onto a larva. Returns actual amount used.

        Hunger resets to zero — the larva is sated and won't signal for
        more food until hunger re-accumulates past the 0.5 threshold
        (~500 ticks / 2.5 sim-days). This prevents a feeding frenzy
        where multiple workers repeatedly feed the same high-hunger
        larva, draining the colony's entire food store in hours.
        """
        if self.stage != LARVA:
            return 0.0
        self.hunger    = 0.0
        self.fed_total += amount
        return amount

    @property
    def alive(self):
        return self.stage != DEAD
