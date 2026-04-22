/* Sim constants — ported from config.py.
 * Single source of truth for all tuning parameters.
 */
#pragma once
#include <cmath>
#include <cstdint>

// ---- Enums ----
enum Face    : uint8_t { FACE_N = 0, FACE_S = 1, FACE_W = 2, FACE_E = 3, FACE_COUNT = 4 };
enum Role    : uint8_t { ROLE_MINOR = 0, ROLE_MAJOR = 1, ROLE_COUNT = 2 };
enum BroodStage : uint8_t { STAGE_EGG = 0, STAGE_LARVA = 1, STAGE_PUPA = 2, STAGE_DEAD = 3 };
enum AntState : uint8_t {
    STATE_IDLE = 0, STATE_TEND_QUEEN = 1, STATE_TEND_BROOD = 2,
    STATE_TO_FOOD = 3, STATE_TO_HOME = 4, STATE_CANNIBALIZE = 5
};

namespace Cfg {

// ---- Time ----
constexpr int TICKS_PER_SIM_DAY = 200;
constexpr int SIM_TPS           = 8;     // sim ticks per second

inline constexpr int days(float n) { return static_cast<int>(n * TICKS_PER_SIM_DAY); }

// ---- Grid ----
constexpr int CHAMBER_WIDTH_PX  = 480;
constexpr int CHAMBER_HEIGHT_PX = 320;
constexpr int CELL_SIZE         = 16;
constexpr int GRID_WIDTH        = CHAMBER_WIDTH_PX / CELL_SIZE;   // 60
constexpr int GRID_HEIGHT       = CHAMBER_HEIGHT_PX / CELL_SIZE;  // 40
constexpr int GRID_CELLS        = GRID_WIDTH * GRID_HEIGHT;       // 2400

// ---- Founding biology ----
constexpr float FOOD_STORE_START = 550.0f;

// ---- Queen ----
constexpr float QUEEN_METABOLISM          = 0.0094f;
constexpr int   QUEEN_LAY_INTERVAL_FOUNDING = days(0.5f);  // 100
constexpr int   QUEEN_LAY_INTERVAL_NORMAL   = days(2.0f);  // 400
constexpr float QUEEN_EGG_FOOD_COST       = 1.08f;
constexpr float QUEEN_LAY_FOOD_FLOOR      = 50.0f;
constexpr float QUEEN_LAY_PRESSURE_MAX    = 0.35f;
constexpr float QUEEN_LAY_SLOWDOWN        = 0.25f;
constexpr float QUEEN_MAX_BROOD_RATIO     = 0.25f;
constexpr int   QUEEN_FOUNDING_EGG_CAP    = 12;
constexpr float QUEEN_HUNGER_RATE         = 0.02f;
constexpr float QUEEN_STARVE_THRESHOLD    = 50.0f;

// ---- Brood ----
constexpr int   EGG_DURATION        = days(10);     // 2000
constexpr float LARVA_HUNGER_RATE   = 0.001f;
constexpr float LARVA_STARVE        = 12.0f;
constexpr float LARVA_FEED_AMOUNT   = 1.44f;
constexpr int   PUPA_DURATION       = days(20);     // 4000

// ---- Role params (indexed by Role enum) ----
struct RoleParams {
    uint8_t move_ticks;
    uint8_t sense_radius;
    float   carry_amount;
    float   metabolism;
    float   speed;              // cells per tick (sub-cell movement)
    int     lifespan_lo, lifespan_hi;
    int     lifespan_pioneer_lo, lifespan_pioneer_hi;
    int     larva_duration;
    float   larva_food_needed;
};

constexpr RoleParams ROLE_PARAMS[ROLE_COUNT] = {
    // ROLE_MINOR
    { 4, 8, 6.6f, 0.00094f, 0.25f,
      days(250), days(400),     // 50000, 80000
      days(200), days(350),     // 40000, 70000
      days(20), 11.5f },        // 4000
    // ROLE_MAJOR
    { 6, 6, 16.5f, 0.00144f, 1.0f / 6.0f,
      days(350), days(550),     // 70000, 110000
      days(200), days(350),     // pioneer (same range for now)
      days(28), 23.0f },        // 5600
};

constexpr Role DEFAULT_BROOD_ROLE = ROLE_MINOR;

// ---- Worker ----
constexpr float WORKER_HUNGER_RATE      = 0.015f;
constexpr float WORKER_STARVE_THRESHOLD = 30.0f;
constexpr int   GATHERING_TRIP_WEAR_LO  = 10;
constexpr int   GATHERING_TRIP_WEAR_HI  = 20;

// ---- Pheromones (JohnBuffer-inspired) ----
constexpr float BASE_MARKER_INTENSITY = 10.0f;
constexpr float MARKER_STEP_DECAY     = 0.02f;
constexpr float PHEROMONE_GRID_DECAY  = 0.997f;
constexpr float PHEROMONE_MAX         = 20.0f;
constexpr float SENSE_FLOOR           = 0.02f;

// ---- Movement ----
constexpr float ARRIVAL_THRESHOLD       = 0.05f;

// ---- Gathering ----
constexpr int   SCOUT_PATIENCE_TICKS    = 3000;
constexpr float FORAGE_DEBUG_PILE_SIZE  = 220.0f;
constexpr float TAP_FEED_AMOUNT        = 200.0f;  // food placed by player tap
constexpr float FOOD_PILE_CAP           = 55.0f;
constexpr int   FOOD_DEPOSIT_RADIUS     = 5;
constexpr int   RETURN_HOME_TICKS       = 960;
constexpr int   IDLE_RECONSIDER_MIN     = 40;
constexpr int   IDLE_RECONSIDER_MAX     = 120;
constexpr int   STALL_THRESHOLD_TICKS   = 12;
constexpr int   ENTRY_ATTRACT_RADIUS    = 8;
constexpr int   CHAMBER_EXPLORE_STEPS   = 150;

// ---- Worker idle/rest ----
constexpr int   IDLE_REST_MIN_TICKS        = 40;    // ~5s at 8 tps
constexpr int   IDLE_REST_MAX_TICKS        = 240;   // ~30s
constexpr int   IDLE_REPOLL_INTERVAL       = 8;     // ~1s between task checks
constexpr float IDLE_DRIFT_SPEED           = 0.1f;  // cells/tick (vs walk 0.25)
constexpr float IDLE_HOLD_WEIGHT           = 0.60f;
constexpr float IDLE_DRIFT_WEIGHT          = 0.25f;
// reface weight = 1 - hold - drift = 0.15
constexpr int   IDLE_MICROSTATE_MIN_TICKS  = 16;    // ~2s
constexpr int   IDLE_MICROSTATE_MAX_TICKS  = 80;    // ~10s
constexpr int   COLONY_MIN_ACTIVE_FOR_IDLE = 10;    // founding-phase suppression
constexpr float IDLE_BUDGET_DAY            = 0.70f;
constexpr float IDLE_BUDGET_TWILIGHT       = 0.85f;
constexpr float IDLE_BUDGET_NIGHT          = 0.95f;

// ---- Metabolic scaling (3/4-power law) ----
constexpr float METABOLIC_SCALE_FLOOR = 0.7f;
constexpr int   METABOLIC_SCALE_ONSET = 10;

inline float metabolic_scale_factor(int population) {
    if (population <= METABOLIC_SCALE_ONSET) return 1.0f;
    float factor = powf(static_cast<float>(population) / METABOLIC_SCALE_ONSET, -0.10f);
    return fmaxf(METABOLIC_SCALE_FLOOR, fminf(1.0f, factor));
}

// ---- Food pressure ----
constexpr float FOOD_PRESSURE_TARGET_DAYS = 7.0f;
constexpr float MIN_GATHERER_FRACTION     = 0.05f;
constexpr float MAX_GATHERER_FRACTION     = 0.80f;

// ---- Starvation cascade ----
constexpr float FAMINE_SLOWDOWN_PRESSURE   = 0.9f;
constexpr float FAMINE_BROOD_CULL_PRESSURE = 0.8f;
constexpr float FAMINE_BROOD_CULL_HUNGER   = 2.0f;
constexpr float QUEEN_PRIORITY_HUNGER      = 20.0f;

// ---- Brood cannibalism ----
constexpr float BROOD_CANNIBALISM_PRESSURE  = 0.95f;
constexpr float BROOD_CANNIBALISM_RECOVERY  = 0.4f;
constexpr float BROOD_CANNIBALISM_MIN_PILE  = 2.0f;
constexpr int   BROOD_CANNIBALISM_COOLDOWN  = 100;

// ---- Recovery bounce ----
constexpr int   RECOVERY_BOOST_DURATION   = 400;
constexpr float RECOVERY_BOOST_THRESHOLD  = 0.8f;

// ---- Chamber geometry ----
constexpr int ENTRY_X[FACE_COUNT] = { GRID_WIDTH/2,  GRID_WIDTH/2,  0,             GRID_WIDTH-1 };
constexpr int ENTRY_Y[FACE_COUNT] = { 0,             GRID_HEIGHT-1, GRID_HEIGHT/2, GRID_HEIGHT/2 };

constexpr int FACE_DX[FACE_COUNT] = {  0,  0, -1,  1 };
constexpr int FACE_DY[FACE_COUNT] = { -1,  1,  0,  0 };

constexpr Face FACE_OPPOSITE[FACE_COUNT] = { FACE_S, FACE_N, FACE_E, FACE_W };

constexpr int QUEEN_SPAWN_X = GRID_WIDTH / 2;   // 30
constexpr int QUEEN_SPAWN_Y = GRID_HEIGHT / 2;  // 20

// ---- Proximity interactions ----
constexpr int   PROXIMITY_DETECTION_RADIUS = 1;
constexpr float PROXIMITY_GREETING_CHANCE  = 0.35f;
constexpr float PROXIMITY_FOOD_SHARE_CHANCE = 0.15f;
constexpr int   GREETING_DURATION_TICKS    = 4;
constexpr int   FOOD_SHARE_DURATION_TICKS  = 88;

// ---- Pool limits ----
constexpr int MAX_LIL_GUYS  = 200;
constexpr int MAX_BROOD      = 100;
constexpr int MAX_FOOD_PILES = 64;

// ---- Food replenishment (single-board mode) ----
constexpr int   FOOD_REPLENISH_INTERVAL = TICKS_PER_SIM_DAY;
constexpr float FOOD_REPLENISH_AMOUNT   = 80.0f;

// ---- Milestone leaves (sprout) ----
constexpr int MILESTONE_LEAF_INTERVAL = 100;  // workers born per new leaf
constexpr int MILESTONE_LEAF_BASE     = 3;    // starting leaf count
constexpr int MILESTONE_LEAF_CAP      = 7;    // max leaves

} // namespace Cfg
