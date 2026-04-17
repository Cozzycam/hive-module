"""Tunable constants for Hive Module.

All numbers are pure data — no pygame, no OS calls. Safe to import from
anywhere in sim/.

Species model: Pheidole / Messor — polymorphic seed-harvesting colony.
Defining traits that drive the sim:
  - Polymorphic workers: MINORS handle brood care and gathering,
    MAJORS defend and mill seeds with oversized heads. Majors only
    emerge once the colony reaches critical mass; Phase 1 founding
    produces exclusively minors.
  - Large claustral queen. She seals herself in, eats nothing, and
    produces her first brood entirely off metabolised wing muscle
    and stored fat. Her metabolism in this period is in low-power
    mode — roughly 1/10 of a gathering worker's.
  - Slow brood cycle: Messor larvae are bulky and take longer to
    develop than most species.
  - Food abstraction still represents seed/fat reserves; seed
    milling and granary chambers arrive with the Phase 2 outworld.

Timescale model
---------------
The fundamental clock is TICKS_PER_SIM_DAY. Every biological timing is
expressed in sim-days and converted to ticks from there. Sim speed is
adjustable at runtime; wall-clock durations scale with it.

Default: 8 ticks/sec and 200 ticks per sim-day → one sim-day every
25 real seconds. The 56-sim-day claustral founding period plays out
in ~23 minutes of wall-clock time. First pioneers eclose around
day 50–51 (~21 minutes in).
"""

# ---------- Time ----------
DISPLAY_FPS          = 60
SIM_SPEED_LEVELS     = [2, 5, 8, 15, 30, 60, 150]   # sim ticks per second
DEFAULT_SPEED_INDEX  = 2                             # = 8 tps (real biology feel)

# Fundamental biological clock. Everything downstream scales from this.
TICKS_PER_SIM_DAY    = 200


def days(n):
    """Helper — convert sim-days to ticks."""
    return int(n * TICKS_PER_SIM_DAY)


# ---------- Display / grid ----------
# Physical target: 480x320 per ESP32 module. Keep the grid tied to this.
CHAMBER_WIDTH_PX  = 480
CHAMBER_HEIGHT_PX = 320
CELL_SIZE         = 16        # pixels per grid cell
GRID_WIDTH        = CHAMBER_WIDTH_PX  // CELL_SIZE   # 60
GRID_HEIGHT       = CHAMBER_HEIGHT_PX // CELL_SIZE   # 40

# ---------- Founding biology ----------
CLAUSTRAL_PERIOD_DAYS = 56
FOOD_STORE_START      = 550.0      # was 500, +10%

# ---------- Queen ----------
QUEEN_METABOLISM        = 0.0094    # -28% from original 0.013
QUEEN_LAY_INTERVAL_FOUNDING = days(0.5)   # fast burst during claustral phase
QUEEN_LAY_INTERVAL_NORMAL  = days(2)     # steady-state post-founding
QUEEN_LAY_INTERVAL         = QUEEN_LAY_INTERVAL_FOUNDING  # initial value
QUEEN_EGG_FOOD_COST     = 1.08     # -28% from original 1.5
QUEEN_LAY_FOOD_FLOOR    = 50.0     # absolute minimum food to allow laying
QUEEN_LAY_PRESSURE_MAX  = 0.35     # queen only breeds when colony has ~9 days
                                    # of reserves (was 0.7 → constant boom-bust)
QUEEN_LAY_SLOWDOWN      = 0.25     # interval doubles above this pressure
QUEEN_MAX_BROOD_RATIO   = 0.25     # pending brood capped at quarter worker count
QUEEN_FOUNDING_EGG_CAP  = 12
QUEEN_HUNGER_RATE       = 0.02
QUEEN_STARVE_THRESHOLD  = 50.0

# ---------- Brood ----------
EGG_DURATION          = days(10)
LARVA_DURATION        = days(20)
LARVA_HUNGER_RATE     = 0.001
LARVA_STARVE          = 12.0
LARVA_FEED_AMOUNT     = 1.44      # -28% from original 2.0
LARVA_MIN_FED_TOTAL   = 11.5     # -28% from original 16.0
PUPA_DURATION         = days(20)

# ---------- Worker roles ----------
ROLE_MINOR = 'minor'
ROLE_MAJOR = 'major'

# Worker metabolism — passive per-tick drain from food_store.
# Minor ~1/10 the queen's rate, matching biological mass ratio.
WORKER_METABOLISM_MINOR  = 0.00094   # -28% from original 0.0013
WORKER_METABOLISM_MAJOR  = 0.00144   # -28% from original 0.002
WORKER_HUNGER_RATE       = 0.015     # per tick when food_store can't cover
WORKER_STARVE_THRESHOLD  = 30.0      # hunger at which worker dies

# Worker lifespan — randomised per individual.
WORKER_LIFESPAN_MINOR    = (days(250), days(400))    # 50k–80k ticks
WORKER_LIFESPAN_MAJOR    = (days(350), days(550))    # 70k–110k ticks
WORKER_LIFESPAN_PIONEER  = (days(200), days(350))    # first 12 workers
GATHERING_TRIP_WEAR       = (10, 20)  # bonus age ticks per completed trip

ROLE_PARAMS = {
    ROLE_MINOR: {
        'move_ticks':        4,
        'speed':             0.25,    # cells per tick (= 1/move_ticks)
        'sense_radius':      8,
        'carry_amount':      6.6,      # was 6.0, +10%
        'metabolism':        WORKER_METABOLISM_MINOR,
        'lifespan':          WORKER_LIFESPAN_MINOR,
        'larva_duration':    days(20),
        'larva_food_needed': 11.5,   # -28% from original
    },
    ROLE_MAJOR: {
        'move_ticks':        6,
        'speed':             1.0 / 6,  # cells per tick (= 1/move_ticks)
        'sense_radius':      6,
        'carry_amount':      16.5,   # +10% from original 15.0
        'metabolism':        WORKER_METABOLISM_MAJOR,
        'lifespan':          WORKER_LIFESPAN_MAJOR,
        'larva_duration':    days(28),
        'larva_food_needed': 23.0,   # -28% from original 32.0
    },
}

DEFAULT_BROOD_ROLE = ROLE_MINOR

WORKER_MOVE_TICKS    = ROLE_PARAMS[ROLE_MINOR]['move_ticks']
WORKER_SENSE_RADIUS  = ROLE_PARAMS[ROLE_MINOR]['sense_radius']
WORKER_CARRY_AMOUNT  = ROLE_PARAMS[ROLE_MINOR]['carry_amount']
WORKER_INITIAL_COUNT = 0

# ---------- Task priorities ----------
PRIO_TEND_QUEEN = 3
PRIO_TEND_BROOD = 2
PRIO_GATHER     = 1

# ---------- Pheromones (JohnBuffer-inspired) ----------
# Distance-based marker system. Workers deposit markers whose intensity
# encodes distance from source via exponential step-decay:
#   intensity = BASE_MARKER_INTENSITY * exp(-MARKER_STEP_DECAY * steps)
#
# Grid cells decay with a single multiplicative factor per tick
# (exponential decay — the v2 fix that solved JohnBuffer's path
# formation issues when he switched from linear).
#
# Two layers: to_home (deposited by outbound workers, followed by
# returning workers) and to_food (deposited by returning workers,
# followed by outbound workers).

ARRIVAL_THRESHOLD      = 0.05    # snap-to-center distance for sub-cell movement

BASE_MARKER_INTENSITY  = 10.0    # deposit strength at 0 steps from source
MARKER_STEP_DECAY      = 0.02   # exp coefficient per step walked
                                  #   30 steps (half a chamber): 55% of base
                                  #   60 steps (one chamber):    30%
                                  #  120 steps (two chambers):    9%
                                  #  180 steps (three chambers):  3%
PHEROMONE_GRID_DECAY   = 0.997   # per-tick multiplicative decay
                                  #   half-life: ~231 ticks (~29 sec at 8 tps)
                                  #   active trails stay strong, abandoned
                                  #   trails fade within ~1-2 minutes
PHEROMONE_MAX          = 20.0    # cap so busy corridors don't blow up

# ---------- Gathering ----------
SCOUT_PATIENCE_TICKS     = 3000
GATHER_DEBUG_PILE_SIZE   = 220.0    # was 200, +10%
TAP_FEED_AMOUNT         = 80.0     # food placed by player tap / touch
FOOD_PILE_CAP            = 55.0    # was 50, +10%
FOOD_DEPOSIT_RADIUS      = 5      # gatherers look this far for a pile to add to
RETURN_HOME_TICKS        = 960    # ~120 sec at 8 tps; workers forced TO_HOME
                                  # after this many ticks outside the queen
                                  # chamber. Prevents workers getting permanently
                                  # stuck in non-queen modules.
IDLE_RECONSIDER_MIN      = 40     # ticks before an IDLE worker re-rolls the
IDLE_RECONSIDER_MAX      = 120    # scouting decision. Random per worker so
                                  # departures stagger over ~5–15 sec at
                                  # 8 tps instead of bursting in a wave.
STALL_THRESHOLD_TICKS    = 12     # if a TO_FOOD worker hasn't moved for this
                                  # many movement ticks, the trail led to
                                  # exhausted food — ignore gradient, explore.
ENTRY_ATTRACT_RADIUS     = 8      # TO_FOOD workers within this distance of an
                                  # active entry are drawn toward it (~30%
                                  # chance per move tick, so scouts naturally
                                  # discover passages between chambers).
CHAMBER_EXPLORE_STEPS    = 150    # after this many steps in TO_FOOD without
                                  # finding food, deterministically walk to
                                  # the nearest exit to search adjacent chambers.

# ---------- Metabolic scaling (¾-power law) ----------
# Larger colonies are more efficient per capita. A colony 10× larger
# needs ~5.6× the food, not 10×. Small founding colonies pay full price.
METABOLIC_SCALE_FLOOR     = 0.7    # minimum multiplier at large colony sizes
METABOLIC_SCALE_ONSET     = 10     # population below which no scaling applies


def metabolic_scale_factor(population):
    """Per-capita metabolism multiplier (0.7–1.0).
    Pop ≤ 10: 1.0.  Pop ~50: ~0.85.  Pop ~200: ~0.75.  Pop 500+: 0.70.
    Exponent -0.10 fitted to match ¾-power law target values;
    the raw N^(-0.25) from Kleiber's law is too aggressive for our
    population range."""
    if population <= METABOLIC_SCALE_ONSET:
        return 1.0
    factor = (population / METABOLIC_SCALE_ONSET) ** (-0.10)
    return max(METABOLIC_SCALE_FLOOR, min(1.0, factor))


# ---------- Food pressure (colony-level gathering regulation) ----------
# Colony targets a buffer of this many sim-days of food. Below that,
# food_pressure rises toward 1.0 and more workers deploy to gather.
FOOD_PRESSURE_TARGET_DAYS = 7.0
MIN_GATHERER_FRACTION     = 0.05   # gathering floor when overstocked
MAX_GATHERER_FRACTION     = 0.80   # gathering ceiling when desperate

# ---------- Starvation cascade ----------
FAMINE_SLOWDOWN_PRESSURE  = 0.9    # pressure above which workers halve speed
FAMINE_BROOD_CULL_PRESSURE = 0.8   # pressure above which hungry larvae die
FAMINE_BROOD_CULL_HUNGER  = 2.0    # larva hunger threshold for culling (~2000 ticks unfed)
                                    # was 0.5 — culled larvae that were barely hungry
QUEEN_PRIORITY_HUNGER     = 20.0   # queen hunger above which workers feed her first

# ---------- Brood cannibalism ----------
BROOD_CANNIBALISM_PRESSURE  = 0.95   # food_pressure threshold to trigger
BROOD_CANNIBALISM_RECOVERY  = 0.4    # fraction of fed_total recovered as food
BROOD_CANNIBALISM_MIN_PILE  = 2.0    # minimum food pile created
BROOD_CANNIBALISM_COOLDOWN  = 100    # ticks between events per chamber

# ---------- Recovery bounce ----------
RECOVERY_BOOST_DURATION     = 400    # ticks of boosted gathering after famine
RECOVERY_BOOST_THRESHOLD    = 0.8    # pressure must have exceeded this

# Legacy aliases — these are no longer used by the new pheromone
# system but may be referenced by older code paths. Safe to remove
# once everything is confirmed working.
PATH_JITTER              = 0.18
PHEROMONE_DEPOSIT_OUT    = 0.6
PHEROMONE_DEPOSIT_RETURN = 1.8
PHEROMONE_SENSE_NOISE    = 0.05
PHEROMONE_JITTER         = 0.18
FOOD_SCENT_SENSE_RADIUS  = 8
EXPLORER_CHANCE          = 0.10
EXPLORER_DEVIATE         = 0.20

# ---------- Event bus ----------
EVENT_BUS_CAPACITY           = 256     # ring buffer slots, overwrites oldest

# ---------- Proximity interactions ----------
# When two lil guys occupy the same or adjacent cell, they may interact.
# Probabilities are per eligible pair per tick.
PROXIMITY_DETECTION_RADIUS   = 1       # Manhattan distance for interaction
PROXIMITY_GREETING_CHANCE    = 0.35    # ~1 in 3 adjacent pairs greet per tick
PROXIMITY_FOOD_SHARE_CHANCE  = 0.15    # when one has food, the other doesn't
GREETING_DURATION_TICKS      = 4       # renderer hint (~0.5 sec at 8 tps)
FOOD_SHARE_DURATION_TICKS    = 88      # renderer hint (~11 sec at 8 tps)

# ---------- Chamber geometry ----------
ENTRY_POINTS = {
    'N': (GRID_WIDTH // 2, 0),
    'S': (GRID_WIDTH // 2, GRID_HEIGHT - 1),
    'W': (0, GRID_HEIGHT // 2),
    'E': (GRID_WIDTH - 1, GRID_HEIGHT // 2),
}
QUEEN_SPAWN = (GRID_WIDTH // 2, GRID_HEIGHT // 2)

FACE_DELTAS = {
    'N': (0, -1),
    'S': (0,  1),
    'W': (-1, 0),
    'E': (1,  0),
}
FACE_OPPOSITE = {'N': 'S', 'S': 'N', 'W': 'E', 'E': 'W'}

# ---------- Multi-module layout ----------
LAYOUT_COLS      = 3
LAYOUT_ROWS      = 3
LAYOUT_GAP_PX    = 4
FOUNDING_POS     = (LAYOUT_COLS // 2, LAYOUT_ROWS // 2)

PANEL_SCALE      = 0.75
EDGE_CLICK_DEPTH = 24
