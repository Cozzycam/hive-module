"""Tunable constants for Hive Module.

All numbers are pure data — no pygame, no OS calls. Safe to import from
anywhere in sim/.

Species model: Pheidole / Messor — polymorphic seed-harvesting ants.
Defining traits that drive the sim:
  - Polymorphic workers: MINORS handle brood care and foraging,
    MAJORS defend and mill seeds with oversized heads. Majors only
    emerge once the colony reaches critical mass; Phase 1 founding
    produces exclusively minors.
  - Large claustral queen. She seals herself in, eats nothing, and
    produces her first brood entirely off metabolised wing muscle
    and stored fat. Her metabolism in this period is in low-power
    mode — roughly 1/10 of a foraging worker's.
  - Slow brood cycle: Messor larvae are bulky and take longer to
    develop than most ants.
  - Food abstraction still represents seed/fat reserves; seed
    milling and granary chambers arrive with the Phase 2 outworld.

Timescale model
---------------
The fundamental clock is TICKS_PER_SIM_DAY. Every biological timing is
expressed in sim-days and converted to ticks from there. Sim speed is
adjustable at runtime; wall-clock durations scale with it.

Default: 8 ticks/sec and 200 ticks per sim-day → one sim-day every
25 real seconds. The 56-sim-day claustral founding period plays out
in ~23 minutes of wall-clock time. First nanitics eclose around
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
CELL_SIZE         = 8         # pixels per grid cell
GRID_WIDTH        = CHAMBER_WIDTH_PX  // CELL_SIZE   # 60
GRID_HEIGHT       = CHAMBER_HEIGHT_PX // CELL_SIZE   # 40

# ---------- Founding biology ----------
CLAUSTRAL_PERIOD_DAYS = 56
FOOD_STORE_START      = 500.0

# ---------- Queen ----------
QUEEN_METABOLISM        = 0.013
QUEEN_LAY_INTERVAL      = days(0.5)
QUEEN_EGG_FOOD_COST     = 1.5      # per individual egg (batch of 6 = 9.0)
QUEEN_LAY_FOOD_FLOOR    = 50.0
QUEEN_FOUNDING_EGG_CAP  = 12
QUEEN_HUNGER_RATE       = 0.02
QUEEN_STARVE_THRESHOLD  = 50.0

# ---------- Brood ----------
EGG_DURATION          = days(10)
LARVA_DURATION        = days(20)
LARVA_HUNGER_RATE     = 0.001
LARVA_STARVE          = 12.0
LARVA_FEED_AMOUNT     = 2.0
LARVA_MIN_FED_TOTAL   = 16.0
PUPA_DURATION         = days(20)

# ---------- Worker castes ----------
CASTE_MINOR = 'minor'
CASTE_MAJOR = 'major'

# Worker metabolism — passive per-tick drain from food_store.
# Minor ~1/10 the queen's rate, matching biological mass ratio.
WORKER_METABOLISM_MINOR  = 0.0013    # per tick (= 0.26/sim-day)
WORKER_METABOLISM_MAJOR  = 0.002     # per tick (= 0.40/sim-day)
WORKER_HUNGER_RATE       = 0.015     # per tick when food_store can't cover
WORKER_STARVE_THRESHOLD  = 30.0      # hunger at which worker dies

# Worker lifespan — randomised per individual.
WORKER_LIFESPAN_MINOR    = (days(250), days(400))    # 50k–80k ticks
WORKER_LIFESPAN_MAJOR    = (days(350), days(550))    # 70k–110k ticks
WORKER_LIFESPAN_NANITIC  = (days(200), days(350))    # first 12 workers
FORAGING_TRIP_WEAR       = (10, 20)  # bonus age ticks per completed trip

CASTE_PARAMS = {
    CASTE_MINOR: {
        'move_ticks':        4,
        'sense_radius':      8,
        'carry_amount':      6.0,
        'metabolism':        WORKER_METABOLISM_MINOR,
        'lifespan':          WORKER_LIFESPAN_MINOR,
        'larva_duration':    days(20),
        'larva_food_needed': 16.0,
    },
    CASTE_MAJOR: {
        'move_ticks':        6,
        'sense_radius':      6,
        'carry_amount':      15.0,
        'metabolism':        WORKER_METABOLISM_MAJOR,
        'lifespan':          WORKER_LIFESPAN_MAJOR,
        'larva_duration':    days(28),
        'larva_food_needed': 32.0,
    },
}

DEFAULT_BROOD_CASTE = CASTE_MINOR

WORKER_MOVE_TICKS    = CASTE_PARAMS[CASTE_MINOR]['move_ticks']
WORKER_SENSE_RADIUS  = CASTE_PARAMS[CASTE_MINOR]['sense_radius']
WORKER_CARRY_AMOUNT  = CASTE_PARAMS[CASTE_MINOR]['carry_amount']
WORKER_INITIAL_COUNT = 0

# ---------- Task priorities ----------
PRIO_TEND_QUEEN = 3
PRIO_TEND_BROOD = 2
PRIO_FORAGE     = 1

# ---------- Pheromones (JohnBuffer-inspired) ----------
# Distance-based marker system. Ants deposit markers whose intensity
# encodes distance from source via exponential step-decay:
#   intensity = BASE_MARKER_INTENSITY * exp(-MARKER_STEP_DECAY * steps)
#
# Grid cells decay with a single multiplicative factor per tick
# (exponential decay — the v2 fix that solved JohnBuffer's path
# formation issues when he switched from linear).
#
# Two layers: to_home (deposited by outbound ants, followed by
# returning ants) and to_food (deposited by returning ants, followed
# by outbound ants).

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

# ---------- Foraging ----------
SCOUT_PATIENCE_TICKS     = 3000
FORAGE_DEBUG_PILE_SIZE   = 200.0
FOOD_PILE_CAP            = 50.0    # max per pile; forces spatial spread
FOOD_DEPOSIT_RADIUS      = 5      # foragers look this far for a pile to add to
RETURN_HOME_TICKS        = 960    # ~120 sec at 8 tps; ants forced TO_HOME
                                  # after this many ticks outside the queen
                                  # chamber. Prevents ants getting permanently
                                  # stuck in non-queen modules.
IDLE_RECONSIDER_MIN      = 40     # ticks before an IDLE ant re-rolls the
IDLE_RECONSIDER_MAX      = 120    # scouting decision. Random per ant so
                                  # departures stagger over ~5–15 sec at
                                  # 8 tps instead of bursting in a wave.
STALL_THRESHOLD_TICKS    = 12     # if a TO_FOOD ant hasn't moved for this
                                  # many movement ticks, the trail led to
                                  # exhausted food — ignore gradient, explore.

# ---------- Food pressure (colony-level foraging regulation) ----------
# Colony targets a buffer of this many sim-days of food. Below that,
# food_pressure rises toward 1.0 and more workers deploy to forage.
FOOD_PRESSURE_TARGET_DAYS = 7.0
MIN_FORAGER_FRACTION      = 0.05   # foraging floor when overstocked
MAX_FORAGER_FRACTION      = 0.50   # foraging ceiling when desperate

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
