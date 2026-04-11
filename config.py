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
# A claustral queen survives the full claustral period (~8 weeks)
# without ingesting anything. Her food store represents metabolised
# wing-muscle + fat reserves. Size it so it clearly lasts the full
# period with a tail of ~20-30% left at first nanitic eclosion.
CLAUSTRAL_PERIOD_DAYS = 56
FOOD_STORE_START      = 500.0

# ---------- Queen ----------
# Claustral low-power metabolism — ~1/10 of a foraging worker. Tuned
# so passive drain across the 56-day period burns roughly 30% of
# reserves (~150 of 500). Egg laying and brood feeding account for the
# rest; see docstring for the full budget.
QUEEN_METABOLISM        = 0.013
QUEEN_LAY_INTERVAL      = days(0.5)   # 100 ticks — a lay every half sim-day
QUEEN_EGG_FOOD_COST     = 3.0
QUEEN_LAY_FOOD_FLOOR    = 50.0        # refuse to lay if reserves run this low
                                       # (conservative — preserves food for
                                       # survival metabolism and brood care
                                       # when pantry is shallow)
QUEEN_FOUNDING_EGG_CAP  = 12          # first-batch egg cap until nanitics emerge
QUEEN_HUNGER_RATE       = 0.02        # accrual per tick when food store is empty
QUEEN_STARVE_THRESHOLD  = 50.0        # queen dies when hunger exceeds this

# ---------- Brood ----------
# Durations in sim-days. Messor is slow — long larva + pupa stages.
# Egg     10 days
# Larva   20 days (minor default)
# Pupa    20 days
# Per-brood total: ~50 days, leaving buffer within the 56-day claustral
# period so the last-laid egg still ecloses before the queen's
# reserves hit critical.
EGG_DURATION          = days(10)   # 2000 ticks
LARVA_DURATION        = days(20)   # 4000 ticks (minor default)
LARVA_HUNGER_RATE     = 0.001      # slow — larvae have fat reserves
LARVA_STARVE          = 12.0       # dies if unfed for ~60 days
LARVA_FEED_AMOUNT     = 2.0        # small bites during founding
LARVA_MIN_FED_TOTAL   = 16.0       # minor cumulative feed required to pupate
PUPA_DURATION         = days(20)   # 4000 ticks

# ---------- Worker castes ----------
# Polymorphism is the signature feature of Pheidole/Messor. A brood
# object carries its destined caste from egg through pupa; when the
# pupa hatches, an Ant of that caste emerges with these params.
CASTE_MINOR = 'minor'
CASTE_MAJOR = 'major'

CASTE_PARAMS = {
    CASTE_MINOR: {
        'move_ticks':        4,          # cooldown between grid steps
        'sense_radius':      8,          # grid cells
        'carry_amount':      4.0,        # food per trophallaxis trip
        'larva_duration':    days(20),
        'larva_food_needed': 16.0,
    },
    CASTE_MAJOR: {
        'move_ticks':        6,          # majors are slower
        'sense_radius':      6,          # shorter range
        'carry_amount':      7.0,        # bigger crop
        'larva_duration':    days(28),   # majors spend longer as larvae
        'larva_food_needed': 32.0,       # much more food to grow
    },
}

# Phase 1 founding: queen lays only minor-destined eggs. Phase 2 will
# enable major production once population exceeds a threshold.
DEFAULT_BROOD_CASTE = CASTE_MINOR

# Legacy worker aliases — still referenced in places that don't care
# about caste. Point to the minor defaults.
WORKER_MOVE_TICKS    = CASTE_PARAMS[CASTE_MINOR]['move_ticks']
WORKER_SENSE_RADIUS  = CASTE_PARAMS[CASTE_MINOR]['sense_radius']
WORKER_CARRY_AMOUNT  = CASTE_PARAMS[CASTE_MINOR]['carry_amount']
WORKER_INITIAL_COUNT = 0                 # founding chamber has no workers at t=0

# ---------- Task priorities (higher = more urgent) ----------
# Workers pick the highest-priority task they can sense within range.
PRIO_TEND_QUEEN = 3
PRIO_TEND_BROOD = 2
PRIO_FORAGE     = 1                      # no-op in Phase 1, reserved for Phase 2

# ---------- Pheromones ----------
# Two trail scents — home (deposited by outbound ants, followed by
# returning ants) and food (deposited by returning ants, followed by
# outbound ants). Trails decay multiplicatively; unreinforced paths
# fade to nothing within ~1-2 sim-minutes at default 8 tps.
#
# Banded decay: strong trails (above HIGH threshold) decay very slowly
# so active corridors stay sticky, mid-strength trails decay normally,
# and weak trails (below LOW) decay fast so abandoned paths disappear
# in a visible puff.
PHEROMONE_DECAY_HIGH     = 0.999     # multiplier when cell > HIGH_THRESHOLD
PHEROMONE_DECAY_MID      = 0.995     # multiplier in the middle band
PHEROMONE_DECAY_LOW      = 0.985     # multiplier when cell < LOW_THRESHOLD
PHEROMONE_HIGH_THRESHOLD = 2.0
PHEROMONE_LOW_THRESHOLD  = 0.5
PHEROMONE_MAX            = 8.0       # cap so busy corridors don't blow up
PHEROMONE_DEPOSIT_OUT    = 0.6       # home_scent dropped per outbound step
PHEROMONE_DEPOSIT_RETURN = 1.8       # food_scent dropped per returning step
                                      # (tuned so a single return trip
                                      # pushes nearby cells into the
                                      # high-decay band quickly)
PHEROMONE_SENSE_NOISE    = 0.05      # jitter added to gradient scores
PHEROMONE_JITTER         = 0.18      # chance that a gradient-following step
                                      # is replaced with a random perpendicular
                                      # step, to keep trail-following from
                                      # looking like a straight robotic line
PATH_JITTER              = 0.18      # chance that a long-distance walk to a
                                      # specific target cell (e.g. exit entry,
                                      # or a food pile) takes a perpendicular
                                      # jitter step — same reason
FOOD_SCENT_SENSE_RADIUS  = 8         # manhattan radius for "is there a food
                                      # trail nearby?" checks used to recruit
                                      # idle workers out of the nest

# ---------- Foraging ----------
SCOUT_PROBABILITY        = 0.30      # chance per idle task-pick to go scouting
SCOUT_PATIENCE_TICKS     = 3000      # scouts give up and head home after this
                                      # many ticks of unsuccessful wandering
                                      # (~15 sim-days at default 200 t/d).
                                      # Must be long enough that a random-walk
                                      # scout has a realistic chance of finding
                                      # food in a 60x40 chamber.
FORAGE_FOOD_PICKUP       = 10.0      # food taken per visit to a food pile
                                      # (Messor workers carry seeds many
                                      # times their body mass)
MIN_BROOD_FEED_RESERVE   = 40.0      # workers won't draw from pantry to
                                      # feed brood unless the store has at
                                      # least this much — queen gets fed
                                      # first in a food crunch
FORAGE_DEBUG_PILE_SIZE   = 200.0     # food units spawned by the F hotkey
HOME_WORKER_RESERVE      = 0.60      # keep at least this fraction of the
                                      # colony in the queen chamber when brood
                                      # is present — rest of the colony is
                                      # free to scout
EXPLORER_CHANCE          = 0.10      # fraction of ants spawned as explorers
EXPLORER_DEVIATE         = 0.20      # chance an explorer ignores the gradient
                                      # and takes a random step instead

# ---------- Chamber geometry ----------
# Entry points sit at the middle of each edge. A face is inactive
# (entries[face] is None) until a neighbour module attaches, at which
# point entries[face] holds the neighbour's module_id.
ENTRY_POINTS = {
    'N': (GRID_WIDTH // 2, 0),
    'S': (GRID_WIDTH // 2, GRID_HEIGHT - 1),
    'W': (0, GRID_HEIGHT // 2),
    'E': (GRID_WIDTH - 1, GRID_HEIGHT // 2),
}
QUEEN_SPAWN = (GRID_WIDTH // 2, GRID_HEIGHT // 2)

# Face direction vectors in (col, row) grid-layout space and opposites
# used for attaching / handoff.
FACE_DELTAS = {
    'N': (0, -1),
    'S': (0,  1),
    'W': (-1, 0),
    'E': (1,  0),
}
FACE_OPPOSITE = {'N': 'S', 'S': 'N', 'W': 'E', 'E': 'W'}

# ---------- Multi-module layout ----------
# The emulator lays chambers out on a fixed grid. Phase 2 first cut
# uses a 3x3 grid with the founding chamber pinned to the centre —
# up to 8 other chambers can attach around it (corners reachable by
# chaining). Bigger layouts come later.
LAYOUT_COLS      = 3
LAYOUT_ROWS      = 3
LAYOUT_GAP_PX    = 4                 # logical-pixel gap between panels
FOUNDING_POS     = (LAYOUT_COLS // 2, LAYOUT_ROWS // 2)   # (col, row) for M0

# How much the native 480x320 panel is scaled down on screen. Keep the
# sim/panel coordinates native; scaling is applied only at blit time
# and clicks are converted back into panel-local coords before hit
# testing. 0.75 fits a full 3x3 grid comfortably on a 1080p display.
PANEL_SCALE      = 0.75

# Any click within this many *logical* pixels of a chamber's edge is
# treated as "attach a neighbour on that face". Gives a generous click
# target without needing a visible button.
EDGE_CLICK_DEPTH = 24
