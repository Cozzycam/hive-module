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
    STATE_TO_FOOD = 3, STATE_TO_HOME = 4, STATE_CANNIBALIZE = 5,
    STATE_ZOOMIES = 6
};

namespace Cfg {

// ---- Grid ----
constexpr int CHAMBER_WIDTH_PX  = 480;
constexpr int CHAMBER_HEIGHT_PX = 320;
constexpr int CELL_SIZE         = 16;
constexpr int GRID_WIDTH        = CHAMBER_WIDTH_PX / CELL_SIZE;   // 30
constexpr int GRID_HEIGHT       = CHAMBER_HEIGHT_PX / CELL_SIZE;  // 20
constexpr int GRID_CELLS        = GRID_WIDTH * GRID_HEIGHT;       // 600

// ---- Founding biology ----
constexpr float FOOD_STORE_START = 80.0f;

// ---- Lifecycle timing (real-time days) ----
constexpr float SECS_PER_DAY = 86400.0f;
constexpr float WORKER_LIFESPAN_MEAN  = 21.0f;   // days
constexpr float WORKER_LIFESPAN_SD    = 3.0f;    // days
constexpr float PIONEER_LIFESPAN_MEAN = 14.0f;   // days
constexpr float PIONEER_LIFESPAN_SD   = 2.0f;    // days

constexpr float EGG_DURATION_DAYS     = 1.0f;
constexpr float LARVA_DURATION_DAYS   = 2.0f;
constexpr float PUPA_DURATION_DAYS    = 1.0f;

// ---- Food rates (per real day) ----
constexpr float QUEEN_FOOD_PER_DAY    = 3.0f;
constexpr float WORKER_FOOD_PER_DAY   = 2.0f;
constexpr float LARVA_FOOD_PER_DAY    = 1.5f;
constexpr float EGG_FOOD_COST         = 1.0f;    // food per egg laid
constexpr float LARVA_TOTAL_FOOD      = LARVA_FOOD_PER_DAY * LARVA_DURATION_DAYS; // 3.0

// ---- Queen laying (real-time) ----
constexpr int   FOUNDING_EGG_COUNT        = 10;
constexpr float FOUNDING_EGG_WINDOW_DAYS  = 0.5f;   // 12 hours
constexpr float ESTABLISHED_LAY_RATE_BASE = 6.0f;   // eggs/day minimum
constexpr float ESTABLISHED_LAY_RATE_MAX  = 10.0f;  // eggs/day at high pop
constexpr float LAY_RATE_POP_SCALE        = 100.0f;
constexpr float LAY_PRESSURE_FLOOR        = 0.20f;  // never taper below 20%

// ---- Starvation (real-time) ----
constexpr float WORKER_SURVIVAL_DAYS  = 1.5f;
constexpr float QUEEN_SURVIVAL_DAYS   = 2.0f;

// ---- Queen ----
constexpr float QUEEN_LAY_FOOD_FLOOR      = 50.0f;
constexpr float QUEEN_LAY_PRESSURE_MAX    = 0.35f;
constexpr float QUEEN_MAX_BROOD_RATIO     = 0.25f;

// ---- Role params (indexed by Role enum) ----
struct RoleParams {
    uint8_t move_ticks;
    uint8_t sense_radius;
    float   carry_amount;
    float   speed;
};

constexpr RoleParams ROLE_PARAMS[ROLE_COUNT] = {
    { 4, 8, 6.6f, 0.25f },        // ROLE_MINOR
    { 6, 6, 16.5f, 1.0f / 6.0f }, // ROLE_MAJOR
};

constexpr Role DEFAULT_BROOD_ROLE = ROLE_MINOR;

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
constexpr float IDLE_DRIFT_SPEED           = 0.05f; // cells/tick (vs walk 0.25)
constexpr float IDLE_HOLD_WEIGHT           = 0.40f;
constexpr float IDLE_DRIFT_WEIGHT          = 0.20f;
constexpr float IDLE_HUDDLE_WEIGHT         = 0.25f;  // drift toward nearest idler/queen
// remaining ~15% = reface
// reface weight = 1 - hold - drift = 0.15
constexpr int   IDLE_MICROSTATE_MIN_TICKS  = 16;    // ~2s
constexpr int   IDLE_MICROSTATE_MAX_TICKS  = 80;    // ~10s
constexpr int   COLONY_MIN_ACTIVE_FOR_IDLE = 10;    // founding-phase suppression
constexpr float IDLE_BUDGET_DAY            = 0.70f;
constexpr float IDLE_BUDGET_TWILIGHT       = 0.85f;
constexpr float IDLE_BUDGET_NIGHT          = 0.95f;

// ---- Zoomies (daytime chase behavior) ----
constexpr float ZOOMIE_CHANCE              = 0.03f;  // per proximity pair per tick
constexpr float ZOOMIE_THIRD_CHANCE        = 0.30f;  // chance to recruit a 3rd lil guy
constexpr float ZOOMIE_SPEED_MULT          = 4.0f;   // speed multiplier vs normal
constexpr int   ZOOMIE_MIN_TICKS           = 16;     // ~2s at 8 tps
constexpr int   ZOOMIE_MAX_TICKS           = 24;     // ~3s at 8 tps

// ---- Metabolic scaling (3/4-power law) ----
constexpr float METABOLIC_SCALE_FLOOR = 0.7f;
constexpr int   METABOLIC_SCALE_ONSET = 10;

inline float metabolic_scale_factor(int population) {
    if (population <= METABOLIC_SCALE_ONSET) return 1.0f;
    float factor = powf(static_cast<float>(population) / METABOLIC_SCALE_ONSET, -0.10f);
    return fmaxf(METABOLIC_SCALE_FLOOR, fminf(1.0f, factor));
}

// ---- Food pressure ----
constexpr float FOOD_PRESSURE_BUFFER_DAYS = 2.0f;
constexpr float MIN_GATHERER_FRACTION     = 0.05f;
constexpr float MAX_GATHERER_FRACTION     = 0.80f;

// ---- Starvation cascade ----
constexpr float FAMINE_SLOWDOWN_PRESSURE   = 0.9f;
constexpr float FAMINE_BROOD_CULL_PRESSURE = 0.8f;
constexpr float FAMINE_BROOD_CULL_HUNGER   = 0.3f;   // normalized 0-1
constexpr float QUEEN_PRIORITY_HUNGER      = 0.3f;   // normalized 0-1

// ---- Brood cannibalism ----
constexpr float BROOD_CANNIBALISM_PRESSURE  = 0.95f;
constexpr float BROOD_CANNIBALISM_RECOVERY  = 0.4f;
constexpr float BROOD_CANNIBALISM_MIN_PILE  = 2.0f;
constexpr float CANNIBALISM_COOLDOWN_SECS   = 21600.0f; // 6 hours

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
constexpr int QUEEN_BODY_HALF_W = 4;            // cells — idle ants won't enter (x)
constexpr int QUEEN_BODY_HALF_H = 5;            // cells — idle ants won't enter (y)

// ---- Stack weights & capacities ----
constexpr int STACK_WEIGHT_PIONEER     = 1;
constexpr int STACK_WEIGHT_MINOR       = 2;
constexpr int STACK_WEIGHT_MAJOR       = 3;
constexpr int STACK_CAPACITY_PIONEER   = 5;   // light — topples fast
constexpr int STACK_CAPACITY_MINOR     = 7;
constexpr int STACK_CAPACITY_MAJOR     = 10;
constexpr int STACK_TOPPLE_TICKS       = 12;  // wobble duration before scatter
constexpr int STACK_FALL_TICKS         = 8;   // fall-down duration after wobble
constexpr uint32_t STACK_COLLAPSE_COOLDOWN_MS = 120000; // 120s before restacking

// ---- Proximity interactions ----
constexpr int   PROXIMITY_DETECTION_RADIUS = 1;
constexpr float BASE_FORAGER_FRACTION      = 0.10f;  // foraging prob at pressure 0.0
constexpr float PROXIMITY_GREETING_CHANCE  = 0.20f;
constexpr float PROXIMITY_FOOD_SHARE_CHANCE = 0.08f;
constexpr int   GREETING_DURATION_TICKS    = 12;   // ~1.5s at 8 tps
constexpr int   FOOD_SHARE_DURATION_TICKS  = 12;   // ~1.5s at 8 tps
constexpr int   INTERACTION_COOLDOWN_TICKS = 40;    // ~5s at 8 tps

// ---- Pool limits ----
constexpr int MAX_LIL_GUYS  = 200;
constexpr int MAX_BROOD      = 100;
constexpr int MAX_FOOD_PILES = 64;

// ---- Milestone leaves (sprout) ----
constexpr int MILESTONE_LEAF_INTERVAL = 100;  // workers born per new leaf
constexpr int MILESTONE_LEAF_BASE     = 3;    // starting leaf count
constexpr int MILESTONE_LEAF_CAP      = 7;    // max leaves

} // namespace Cfg
