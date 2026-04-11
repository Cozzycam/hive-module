"""Pheromone grid — JohnBuffer-inspired scalar marker system.

Two scalar layers:
  - to_home: deposited by ants walking AWAY from the colony (TO_FOOD
    state). Intensity decreases with distance from colony via
    exponential step-decay in the deposit function. Returning ants
    follow this gradient to find their way home.
  - to_food: deposited by ants carrying food BACK to the colony
    (TO_HOME state). Intensity decreases with distance from food.
    Outbound ants follow this gradient to find known food sources.

Each cell stores a single float per layer. No direction vectors —
the gradient IS the direction. Decay is a uniform multiplicative
factor per tick (true exponential, matching JohnBuffer's v2 fix).

Backing storage is flat Python lists — pure-Python portable to
MicroPython on the ESP32 later. Memory footprint per chamber
(60x40 grid, 2 layers, 1 float per cell): ~19 KB.
"""

import config as C


# Below this value a cell reads as zero — avoids chasing numerical
# dust after decay drives values toward epsilon.
SENSE_FLOOR = 0.02


class PheromoneMap:
    __slots__ = ('width', 'height', '_home', '_food')

    def __init__(self, width, height):
        self.width  = width
        self.height = height
        size = width * height
        self._home = [0.0] * size
        self._food = [0.0] * size

    def _idx(self, x, y):
        return y * self.width + x

    def _in(self, x, y):
        return 0 <= x < self.width and 0 <= y < self.height

    # ---- reads (bounds-safe, return 0 outside the grid) ----

    def home(self, x, y):
        if not self._in(x, y):
            return 0.0
        v = self._home[self._idx(x, y)]
        return v if v > SENSE_FLOOR else 0.0

    def food(self, x, y):
        if not self._in(x, y):
            return 0.0
        v = self._food[self._idx(x, y)]
        return v if v > SENSE_FLOOR else 0.0

    def raw_home(self, x, y):
        """Un-floored read — renderer uses this so weak trails fade
        visually instead of popping off."""
        if not self._in(x, y):
            return 0.0
        return self._home[self._idx(x, y)]

    def raw_food(self, x, y):
        if not self._in(x, y):
            return 0.0
        return self._food[self._idx(x, y)]

    # Compatibility stubs — direction overlay reads these but we no
    # longer store direction vectors. Return (mag, 0, 0) so the
    # overlay filter (direction length > 0.5) skips every cell.
    def vector_home(self, x, y):
        return (self.home(x, y), 0.0, 0.0)

    def vector_food(self, x, y):
        return (self.food(x, y), 0.0, 0.0)

    # ---- writes ----

    def deposit_home(self, x, y, amount):
        """Add to_home scent. Takes the max of existing and new value
        (JohnBuffer style) so multiple ants reinforce but don't
        explode the value."""
        if not self._in(x, y):
            return
        i = self._idx(x, y)
        self._home[i] = min(
            max(self._home[i], amount),
            C.PHEROMONE_MAX,
        )

    def deposit_food(self, x, y, amount):
        """Add to_food scent. See deposit_home."""
        if not self._in(x, y):
            return
        i = self._idx(x, y)
        self._food[i] = min(
            max(self._food[i], amount),
            C.PHEROMONE_MAX,
        )

    # ---- per-tick decay ----

    def decay(self):
        """Single multiplicative decay — one tuning knob. Exponential
        falloff matches JohnBuffer's v2 fix that solved all his path
        formation issues when he switched from linear decay."""
        rate  = C.PHEROMONE_GRID_DECAY
        floor = SENSE_FLOOR
        hm = self._home
        fm = self._food
        for i in range(len(hm)):
            v = hm[i]
            if v > 0.0:
                v *= rate
                hm[i] = v if v > floor else 0.0
        for i in range(len(fm)):
            v = fm[i]
            if v > 0.0:
                v *= rate
                fm[i] = v if v > floor else 0.0
