"""Pheromone grid.

Two vector layers over the chamber grid, one per scent type:
  - home_scent: deposited by outbound ants. Each cell stores a
    magnitude plus a weighted-average flow direction vector. Ants
    could eventually follow home_scent backward to reach the nest
    (wired up in Stage 3 of the vector-field upgrade; for now only
    the magnitude is read, via the scalar home() accessor).
  - food_scent: deposited by returning ants carrying cargo. Same
    vector structure — outbound ants will follow the direction
    forward to reach known food sources once _step_by_score is
    converted to dot-product steering (Stage 2).

Each layer stores three parallel flat lists:
  _<layer>_mag — float, strength of scent at this cell, capped at
                 PHEROMONE_MAX and decayed each tick.
  _<layer>_dx  — float in [-1, 1], x-component of flow direction.
  _<layer>_dy  — float in [-1, 1], y-component of flow direction.

Deposit writes a weighted average of the depositing ant's heading
into the direction components:

    blend = amount / (old_mag + amount)
    new_dx = old_dx * (1 - blend) + ant_dx * blend
    new_dy = old_dy * (1 - blend) + ant_dy * blend
    new_mag = min(old_mag + amount, PHEROMONE_MAX)

A cell with one-way traffic converges toward the depositing ant's
heading. A cell with opposite-direction traffic averages toward
zero, which correctly reads as "busy corridor, no dominant flow".
Consumers should check the direction magnitude (sqrt(dx² + dy²))
before trusting the direction for navigation.

Decay applies to magnitude only. Direction components persist as
long as the cell has any scent. When magnitude falls below
SENSE_FLOOR the cell is fully zeroed so stale directions can't
leak into future deposits.

Backing storage is flat Python lists so this stays pure-Python
portable to MicroPython on the ESP32 later — no numpy.

Memory footprint per chamber (Phase 2, 60x40 grid, 2 layers, 3
floats per cell at ~28 bytes): ~40 KB. On the eventual ESP32/C
port the direction components can be packed as two int8s (-1/0/1)
alongside a uint16 magnitude — 4 bytes per cell per layer, ~19 KB
for both layers. Don't optimise that in the Python sim; just be
aware the storage layout is designed to pack down cleanly.
"""

import config as C


# Below this value, a cell is treated as zero when sensing — avoids
# chasing numerical dust after decay drives values toward zero. Also
# used as the minimum threshold for the renderer overlay and as the
# trigger to wipe stale direction components during decay.
SENSE_FLOOR = 0.02


class PheromoneMap:
    __slots__ = (
        'width', 'height',
        '_home_mag', '_home_dx', '_home_dy',
        '_food_mag', '_food_dx', '_food_dy',
    )

    def __init__(self, width, height):
        self.width  = width
        self.height = height
        size = width * height
        self._home_mag = [0.0] * size
        self._home_dx  = [0.0] * size
        self._home_dy  = [0.0] * size
        self._food_mag = [0.0] * size
        self._food_dx  = [0.0] * size
        self._food_dy  = [0.0] * size

    # ---- indexing ----

    def _idx(self, x, y):
        return y * self.width + x

    def _in(self, x, y):
        return 0 <= x < self.width and 0 <= y < self.height

    # ---- scalar reads (bounds-safe, return 0 outside the grid) ----
    # Backwards-compatible with the pre-vector API: callers that only
    # need "how much scent is here" keep working unchanged.

    def home(self, x, y):
        if not self._in(x, y):
            return 0.0
        v = self._home_mag[self._idx(x, y)]
        return v if v > SENSE_FLOOR else 0.0

    def food(self, x, y):
        if not self._in(x, y):
            return 0.0
        v = self._food_mag[self._idx(x, y)]
        return v if v > SENSE_FLOOR else 0.0

    def raw_home(self, x, y):
        """Un-floored magnitude read — used by the renderer so weak
        trails still fade instead of popping off instantly."""
        if not self._in(x, y):
            return 0.0
        return self._home_mag[self._idx(x, y)]

    def raw_food(self, x, y):
        if not self._in(x, y):
            return 0.0
        return self._food_mag[self._idx(x, y)]

    # ---- vector reads ----

    def vector_home(self, x, y):
        """Return (mag, dx, dy) for the home layer. Magnitude is
        floored at SENSE_FLOOR (same behaviour as the scalar home()
        accessor). Out-of-bounds returns (0, 0, 0)."""
        if not self._in(x, y):
            return (0.0, 0.0, 0.0)
        i = self._idx(x, y)
        m = self._home_mag[i]
        if m <= SENSE_FLOOR:
            return (0.0, 0.0, 0.0)
        return (m, self._home_dx[i], self._home_dy[i])

    def vector_food(self, x, y):
        """(mag, dx, dy) for the food layer. See vector_home()."""
        if not self._in(x, y):
            return (0.0, 0.0, 0.0)
        i = self._idx(x, y)
        m = self._food_mag[i]
        if m <= SENSE_FLOOR:
            return (0.0, 0.0, 0.0)
        return (m, self._food_dx[i], self._food_dy[i])

    # ---- writes ----

    def deposit_home(self, x, y, amount, ant_dx, ant_dy):
        """Add home_scent at (x, y) with the depositing ant's cardinal
        heading. Magnitude is additive and capped; direction is a
        weighted average of the existing direction and the new one."""
        if not self._in(x, y):
            return
        i = self._idx(x, y)
        old_mag = self._home_mag[i]
        denom = old_mag + amount
        if denom <= 0.0:
            return
        blend = amount / denom
        inv   = 1.0 - blend
        self._home_dx[i] = self._home_dx[i] * inv + ant_dx * blend
        self._home_dy[i] = self._home_dy[i] * inv + ant_dy * blend
        new_mag = denom
        if new_mag > C.PHEROMONE_MAX:
            new_mag = C.PHEROMONE_MAX
        self._home_mag[i] = new_mag

    def deposit_food(self, x, y, amount, ant_dx, ant_dy):
        """Add food_scent at (x, y) with the depositing ant's cardinal
        heading. See deposit_home()."""
        if not self._in(x, y):
            return
        i = self._idx(x, y)
        old_mag = self._food_mag[i]
        denom = old_mag + amount
        if denom <= 0.0:
            return
        blend = amount / denom
        inv   = 1.0 - blend
        self._food_dx[i] = self._food_dx[i] * inv + ant_dx * blend
        self._food_dy[i] = self._food_dy[i] * inv + ant_dy * blend
        new_mag = denom
        if new_mag > C.PHEROMONE_MAX:
            new_mag = C.PHEROMONE_MAX
        self._food_mag[i] = new_mag

    # ---- per-tick decay ----

    def decay(self):
        """Banded decay on magnitude:
          - strong trails (above HIGH_THRESHOLD) decay very slowly
          - mid-strength trails decay normally
          - weak trails (below LOW_THRESHOLD) decay fast
        Direction components are NOT decayed; they persist at the same
        orientation as long as the cell retains any scent. When the
        magnitude drops below SENSE_FLOOR the whole cell is zeroed so
        stale directions can't contaminate fresh deposits.
        """
        hm  = self._home_mag
        hdx = self._home_dx
        hdy = self._home_dy
        fm  = self._food_mag
        fdx = self._food_dx
        fdy = self._food_dy
        n = len(hm)
        high  = C.PHEROMONE_DECAY_HIGH
        mid   = C.PHEROMONE_DECAY_MID
        low   = C.PHEROMONE_DECAY_LOW
        hi_t  = C.PHEROMONE_HIGH_THRESHOLD
        lo_t  = C.PHEROMONE_LOW_THRESHOLD
        floor = SENSE_FLOOR
        for i in range(n):
            v = hm[i]
            if v <= 0.0:
                continue
            if v > hi_t:
                v *= high
            elif v < lo_t:
                v *= low
            else:
                v *= mid
            if v <= floor:
                hm[i]  = 0.0
                hdx[i] = 0.0
                hdy[i] = 0.0
            else:
                hm[i] = v
        for i in range(n):
            v = fm[i]
            if v <= 0.0:
                continue
            if v > hi_t:
                v *= high
            elif v < lo_t:
                v *= low
            else:
                v *= mid
            if v <= floor:
                fm[i]  = 0.0
                fdx[i] = 0.0
                fdy[i] = 0.0
            else:
                fm[i] = v
