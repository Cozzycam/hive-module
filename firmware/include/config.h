/* Sim constants — ported from config.py.
 * Single source of truth for all tuning parameters.
 */
#pragma once
#include <cmath>
#include <cstdint>

// ---- Enums ----
enum Face    : uint8_t { FACE_N = 0, FACE_S = 1, FACE_W = 2, FACE_E = 3, FACE_COUNT = 4 };
enum Role    : uint8_t { ROLE_CONKER = 0, ROLE_COUNT = 1 };
// Legacy aliases for persistence compatibility (old records may have role=0 or role=1)
constexpr Role ROLE_MINOR = ROLE_CONKER;
constexpr Role ROLE_MAJOR = ROLE_CONKER;
enum BroodStage : uint8_t { STAGE_EGG = 0, STAGE_SEED = 1, STAGE_DEAD = 2 };
// Legacy alias for persistence/event compatibility
constexpr BroodStage STAGE_LARVA = STAGE_SEED;
constexpr BroodStage STAGE_PUPA  = STAGE_SEED;
enum AntState : uint8_t {
    STATE_IDLE = 0, STATE_TEND_QUEEN = 1, STATE_TEND_BROOD = 2,
    STATE_TO_FOOD = 3, STATE_TO_HOME = 4, STATE_CANNIBALIZE = 5,
    STATE_ZOOMIES = 6,
    STATE_EATING = 7,
    STATE_MOURNING = 8,  // bonded partner died — pay respects at the husk
    STATE_FARMING = 9,   // green thumb at work — off to sow a garden plot
    STATE_CRAFTING = 10  // the muse struck — making something that lasts
};

// Firmware version — bump manually for OTA releases.
// Do NOT use __DATE__/__TIME__ (triggers spurious OTA pushes on every recompile).
constexpr uint32_t FW_VERSION = 182;

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
constexpr float CONKER_LIFESPAN_MEAN  = 21.0f;   // days
constexpr float CONKER_LIFESPAN_SD    = 3.0f;    // days
// Legacy aliases
constexpr float WORKER_LIFESPAN_MEAN  = CONKER_LIFESPAN_MEAN;
constexpr float WORKER_LIFESPAN_SD    = CONKER_LIFESPAN_SD;

constexpr float EGG_DURATION_DAYS     = 1.0f;
constexpr float SEED_DURATION_DAYS    = 2.0f;    // was LARVA(2) + PUPA(1), now single stage
// Legacy aliases
constexpr float LARVA_DURATION_DAYS   = SEED_DURATION_DAYS;

// Normal total development: egg(1) + seed(2) = 3 days
constexpr uint32_t NORMAL_TOTAL_DURATION_MS = 3UL * 86400UL * 1000UL;

// ---- Founder bootstrap (per-egg accelerated development) ----
constexpr int FOUNDER_COHORT_SIZE = 8;
// Target total durations (egg + seed combined) for founding eggs, in ms
constexpr uint32_t FOUNDER_DURATIONS_MS[FOUNDER_COHORT_SIZE] = {
     600000UL,   // egg 0: 10 minutes
    1800000UL,   // egg 1: 30 minutes
    3600000UL,   // egg 2: 1 hour
   14400000UL,   // egg 3: 4 hours
   43200000UL,   // egg 4: 12 hours
   86400000UL,   // egg 5: 24 hours
  172800000UL,   // egg 6: 48 hours
  NORMAL_TOTAL_DURATION_MS,  // egg 7: 72 hours (normal)
};

// ---- Conker size distribution ----
constexpr float CONKER_SCALE_MEAN = 3.2f;   // matches old minor size
constexpr float CONKER_SCALE_SD   = 0.5f;
constexpr float CONKER_SCALE_MIN  = 2.2f;
constexpr float CONKER_SCALE_MAX  = 4.2f;
// Hatchlings render at 60% and grow to full size over their first day
constexpr uint32_t GROWTH_JUVENILE_MS = 86400000UL;  // 24h sim-running time

// ---- Weather-driven challenges ----
// Real local weather (Open-Meteo) auto-starts/ends survival challenges.
// Hysteresis: N consecutive observations (spaced CHECK_MS apart, weather
// refetches every 10 min) so a gusty minute doesn't declare a storm.
constexpr uint32_t WX_CHALLENGE_CHECK_MS    = 5UL * 60UL * 1000UL;       // classify every 5 min
constexpr int      WX_CHALLENGE_START_OBS   = 2;                          // consecutive obs to start
constexpr int      WX_CHALLENGE_CLEAR_OBS   = 2;                          // consecutive calm obs to end
constexpr uint32_t WX_CHALLENGE_MIN_MS      = 15UL * 60UL * 1000UL;       // never end within 15 min
constexpr uint32_t WX_CHALLENGE_COOLDOWN_MS = 3UL * 60UL * 60UL * 1000UL; // per-type re-trigger gap
constexpr float    WX_CHALLENGE_BURN_SCALE  = 0.35f;  // burn mult = 1 + scale * severity

// ---- Garden module role ----
constexpr float GARDEN_CRITTER_MULT  = 3.0f;       // critter spawn bias vs plain chamber

// Farming (garden module only). Crops grow on WALL-CLOCK time (lifecycle
// clock, not sim ticks). Balance target: a well-tended garden covers
// ~40% of a full colony's burn — raises the floor without killing the
// foraging economy.
constexpr int      GARDEN_PLOTS            = 4;
constexpr uint32_t PLANT_STAGE_SECS        = 4 * 3600;  // sprout→growing→mature
constexpr float    PLANT_YIELD             = 5.0f;      // food pile per mature crop
constexpr float    PLANT_RAIN_GROWTH_MULT  = 1.5f;      // rain is good for crops
constexpr float    PLANT_WITHER_CHANCE     = 0.20f;     // per stage during heat/drought challenge
constexpr float    GREEN_THUMB_MIN         = 0.55f;     // aptitude floor to ever sow
constexpr float    SOW_CHANCE_PER_TICK     = 0.0006f;   // idle roll (×green_thumb): ~1 sow per few idle minutes
constexpr int      SOW_DURATION_TICKS      = 24;        // ~3s planting pause
constexpr int      FARM_MAX_TICKS          = 600;       // failsafe: give up the trip after ~75s

// Foraging stands down during deep night below this food pressure —
// the colony sleeps; only genuine hunger sends anyone out in the dark
constexpr float    NIGHT_FORAGE_MIN_PRESSURE = 0.5f;

// ---- Making (artifacts) ----
constexpr int   MAX_ARTWORKS          = 10;      // per module; oldest weathers away
constexpr float MUSE_MIN              = 0.60f;   // aptitude floor to ever craft
constexpr float CRAFT_CHANCE_PER_TICK = 0.0002f; // idle roll (×muse, ×play_surplus)
constexpr float CRAFT_GRIEF_CHANCE    = 0.35f;   // a mourner with muse carves a memorial
constexpr int   CRAFT_DURATION_TICKS  = 1200;    // ~2.5 min of visible work
constexpr int   CRAFT_MAX_TICKS       = 2400;    // failsafe for the whole trip
constexpr float ADMIRE_CHANCE_PER_TICK = 0.002f; // idle near art (×taste)
constexpr float ADMIRE_BOREDOM_RELIEF = 8.0f;
constexpr float ADMIRE_BOND_NUDGE     = 0.010f;  // toward the maker, if alive

// ---- Night life ----
// The night is a register, not an absence: owls potter and firefly-watch,
// sleepers turn over and resettle. Interesting at any hour, any population.
constexpr float OWL_NIGHT_THRESHOLD_LIFT = 0.25f;  // owls resist bedtime after dark
constexpr float OWL_DAY_THRESHOLD_DROP   = 0.15f;  // ...and lie in / siesta by day
constexpr float SLEEP_TWITCH_CHANCE      = 0.0014f; // per tick ≈ one twitch / ~90s asleep

// ---- Food rates (per real day) ----
constexpr float QUEEN_FOOD_PER_DAY    = 3.0f;
constexpr float WORKER_FOOD_PER_DAY   = 2.0f;
constexpr float SEED_FOOD_PER_DAY     = 1.5f;    // was LARVA_FOOD_PER_DAY
constexpr float EGG_FOOD_COST         = 1.0f;    // food per egg laid
constexpr float SEED_TOTAL_FOOD       = SEED_FOOD_PER_DAY * SEED_DURATION_DAYS; // 3.0
// Legacy aliases
constexpr float LARVA_FOOD_PER_DAY    = SEED_FOOD_PER_DAY;
constexpr float LARVA_TOTAL_FOOD      = SEED_TOTAL_FOOD;

// ---- Hunger (individual meal cycle) ----
constexpr float WORKER_HUNGER_PER_DAY = 100.0f;   // 0→100 in 1 day without eating
constexpr float QUEEN_HUNGER_PER_DAY  = 200.0f;   // 0→100 in 0.5 days (needs 2x feeding)
constexpr float WORKER_MEAL_COST      = WORKER_FOOD_PER_DAY / 3.0f;  // ~0.67 per meal
constexpr float QUEEN_MEAL_COST       = QUEEN_FOOD_PER_DAY / 6.0f;   // 0.50 per feeding
constexpr float HUNGER_STARVE         = 100.0f;    // death threshold
constexpr float HUNGER_SLOWDOWN       = 80.0f;     // speed penalty starts

// ---- Queen laying (real-time) ----
constexpr int   FOUNDING_EGG_COUNT        = FOUNDER_COHORT_SIZE;  // founding phase = founder cohort
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
    // carry 0.4u/trip: a tapped pile (2u) takes 5 hauls — repeat journeys
    // are what lay and ratify pheromone trails, and a care package becomes
    // a proper supply column
    { 4, 8, 0.4f, 0.25f },        // ROLE_CONKER
};

constexpr Role DEFAULT_BROOD_ROLE = ROLE_CONKER;

// ---- Pheromones (JohnBuffer-inspired) ----
constexpr float BASE_MARKER_INTENSITY = 10.0f;
constexpr float MARKER_STEP_DECAY     = 0.02f;
constexpr float PHEROMONE_HOME_DECAY  = 0.992f;  // half-life ~11s at 8tps — stale trails clear fast
constexpr float PHEROMONE_FOOD_DECAY  = 0.9985f; // half-life ~58s at 8tps — food trails survive return trips
constexpr float PHEROMONE_MAX         = 20.0f;
constexpr float SENSE_FLOOR           = 0.02f;

// ---- Movement ----
constexpr float ARRIVAL_THRESHOLD       = 0.05f;

// ---- Gathering ----
constexpr int   SCOUT_PATIENCE_TICKS    = 3000;
constexpr float FORAGE_DEBUG_PILE_SIZE  = 220.0f;
constexpr float TAP_FEED_AMOUNT        = 2.0f;   // one tap = one conker-day
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
constexpr int   COLONY_MIN_ACTIVE_FOR_IDLE = 2;     // idle kicks in once 2+ workers exist
constexpr float IDLE_BUDGET_DAY            = 0.70f;
constexpr float IDLE_BUDGET_TWILIGHT       = 0.85f;
constexpr float IDLE_BUDGET_NIGHT          = 0.95f;

// ---- Forage flair (cosmetic character — never overrides trail logic) ----
// All flair is gated off above FLAIR_MAX_PRESSURE so a stressed colony
// forages all-business. Casts/ceremony only fire during blind exploration
// or at the pile — trail following and trail laying stay untouched.
constexpr float FLAIR_MAX_PRESSURE        = 0.5f;
constexpr float CAST_CHANCE               = 0.05f;  // per blind-explore decision
constexpr int   CAST_MAX_PER_TRIP         = 3;
constexpr int   CAST_DURATION_TICKS       = 9;      // ~1s scanning pause
constexpr int   CEREMONY_LINGER_MAX_TICKS = 8;      // picky extra inspect (x food_preference)
constexpr float TRAIL_DEFECT_BASE         = 0.03f;  // everyone occasionally leaves the trail
constexpr float TRAIL_DEFECT_PERS         = 0.22f;  // + exploration scale (3%-25% total)
constexpr float CARRY_WADDLE_AMP_PX       = 1.8f;   // render-only: never alters the cell path
constexpr float CARRY_WADDLE_FREQ         = 1.6f;   // wobble cycles per cell walked
constexpr float FORAGE_TEMPO_SPEED_MIN    = 0.85f;  // low work_tempo amble (outbound only)
constexpr float FORAGE_TEMPO_SPEED_SPAN   = 0.30f;  // up to 1.15x march
constexpr float OVERLOAD_TOPPLE_CHANCE    = 0.04f;  // comic fumble at pickup
constexpr float COURTESY_YIELD_CHANCE     = 0.35f;  // outbound steps aside for a carrier
constexpr int   COURTESY_YIELD_TICKS      = 6;      // ~0.75s pause watching them pass
constexpr int   WAGGLE_DANCE_TICKS        = 16;     // ~2s post-delivery dance
// Waggle recruitment: fresh deliveries (food_delivery_signal 200→0 over ~25s)
// temporarily widen the forager cap — a productive pile swells the swarm,
// and the swarm dissolves naturally once deliveries stop refreshing it.
constexpr float RECRUIT_SIGNAL_FRACTION   = 0.50f;  // extra forager fraction at full signal

// ---- Finger scent trails (swipe the glass to whisper a suggestion) ----
// Deposited into the real food-pheromone layer so foragers follow it with
// the existing trail logic. Deliberately weaker than a real trail (10.0):
// if it leads to food the colony's own return trips ratify it into a road;
// if it leads nowhere it decays as a false rumour.
constexpr int   SCENT_MARK_TTL         = 20;   // shimmer ticks (~2.5s)
constexpr int   MAX_SCENT_MARKS        = 48;

// ---- Fireflies (night ambience + chase bait) ----
constexpr int   MAX_FIREFLIES             = 5;
constexpr float FIREFLY_NIGHT_FACTOR_MIN  = 0.6f;   // dusk onward
constexpr float FIREFLY_SPAWN_CHANCE      = 0.004f; // per empty slot per tick
constexpr float FIREFLY_SPEED             = 0.06f;  // lazy drift, cells/tick
constexpr float FIREFLY_FLEE_SPEED        = 0.22f;  // when a conker closes in
constexpr float FIREFLY_CHASE_CHANCE      = 0.04f;  // per idle repoll at night
constexpr int   FIREFLY_CHASE_RADIUS      = 8;      // cells, manhattan
constexpr int   FIREFLY_CHASE_MAX_TICKS   = 64;     // ~8s before losing interest
constexpr float FIREFLY_CHASE_SPEED_MULT  = 2.0f;   // night dash (vs zoomies 4x)

// ---- Mourning & elders ----
constexpr float MOURN_MIN_BOND        = 0.25f;  // bond strength to grieve
constexpr int   MOURN_MAX_PARTNERS    = 3;      // closest bonds only
constexpr int   MOURN_DURATION_TICKS  = 300;    // ~37s vigil for a best friend (reciprocated)
constexpr int   MOURN_ONEWAY_TICKS    = 100;    // ~12s — a one-way bond grieves only briefly
constexpr int   ELDER_HUDDLE_PULL     = 4;      // idlers prefer huddling near elders
                                                // (distance bias, cells)

// ---- Friendship behaviour (bonds pull on everyday life) ----
constexpr int   FRIEND_HUDDLE_PULL    = 6;      // idlers prefer huddling near friends
                                                // (best friends pull 2x this), cells
constexpr float BOND_PLAY_JOIN_MULT   = 3.0f;   // friends are likelier to join a friend's game
constexpr float BOND_PLAY_PAIR_MULT   = 2.5f;   // friends are likelier to kick off a game together
constexpr float BOND_FORAGE_FOLLOW    = 0.18f;  // chance/step a wandering forager drifts toward
                                                // a foraging friend (shared trails)

// v152: friendship is activity-led. Sociability gates how fast a conker bonds
// (shared by the proximity engine and the per-activity bonuses below); loners
// below the floor never bond, so circles stay small without any hard friend cap.
constexpr float BOND_LONER_FLOOR      = 0.22f;  // social personality below this → never bonds
constexpr float BOND_SOCIAL_GAIN      = 1.7f;   // sociability → bond-rate slope (×0..1.5)
// Doing something fun together is the main way friendships form now — a direct
// nudge per shared moment (scaled per-conker by sociability). Passive proximity
// (in _bond_detect_proximity) is now just a slow trickle on top of these.
constexpr float BOND_ACT_ZOOMIE       = 0.040f; // started a chase/parade together
constexpr float BOND_ACT_JOIN         = 0.030f; // joined a friend's game
constexpr float BOND_ACT_STACK        = 0.050f; // climbed into a greeting tower
constexpr float BOND_ACT_GROOM        = 0.040f; // mutual grooming

// ---- Zoomies (daytime chase behavior) ----
constexpr float ZOOMIE_CHANCE              = 0.03f;  // per proximity pair per tick
constexpr float ZOOMIE_THIRD_CHANCE        = 0.30f;  // chance to recruit a 3rd lil guy
constexpr float ZOOMIE_SPEED_MULT          = 4.0f;   // speed multiplier vs normal
constexpr int   ZOOMIE_MIN_TICKS           = 16;     // ~2s at 8 tps
constexpr int   ZOOMIE_MAX_TICKS           = 24;     // ~3s at 8 tps

// ---- Larder (food storage cap) ----
// All food is player-given; the colony can only store so much. Excess
// spoils, so feeding stays meaningful and stockpiles can't trivialise
// months. A full larder = ~2 weeks of neglect tolerance.
constexpr float LARDER_CAP_DAYS        = 14.0f;

// ---- Surplus playtime ----
// A deep pantry frees the colony to play: foraging winds down and play
// behaviours scale up with play_surplus() (0 at MIN_DAYS of food, 1 at MAX).
// Tuned to the larder cap: a full larder is a festival.
constexpr float PLAY_SURPLUS_MIN_DAYS  = 4.0f;
constexpr float PLAY_SURPLUS_MAX_DAYS  = 12.0f;
constexpr float SURPLUS_FORAGE_DAMP    = 0.7f;   // max forager-fraction reduction
constexpr float ZOOMIE_SURPLUS_BOOST   = 2.0f;   // extra zoomie chance at full surplus
constexpr int   ZOOMIE_SURPLUS_TICKS   = 24;     // extra max duration at full surplus
constexpr float PARADE_CHANCE          = 0.35f;  // playful encounter becomes a parade
constexpr float PARADE_MIN_SURPLUS     = 0.30f;  // parades need a comfortable pantry
constexpr float PARADE_SPEED_MULT      = 1.4f;   // gentle trot, not a sprint
constexpr int   PARADE_MIN_TICKS       = 64;     // ~8s at 8 tps
constexpr int   PARADE_MAX_TICKS       = 112;    // ~14s at 8 tps
constexpr int   PARADE_MAX_FOLLOWERS   = 4;
constexpr int   PARADE_RECRUIT_RADIUS  = 4;      // cells, manhattan
constexpr float PIROUETTE_CHANCE       = 0.12f;  // idle microstate roll at full surplus
constexpr int   PIROUETTE_TICKS        = 12;     // ~1.5s twirl
constexpr float JOY_SPRINT_CHANCE      = 0.003f; // per idle tick at full surplus
constexpr float ZOOMIE_JOIN_CHANCE     = 0.5f;   // bystander swept into a passing game
constexpr int   MAX_CONCURRENT_PLAY    = 2;      // solo launches wait their turn —
                                                 // keeps a big colony from stampeding

// ---- Needs / mood (v114) ----
// The needs framework is wired up whole, but NEEDS_ACTIVE_MASK gates which
// drives actually evolve so each can be activated and tuned on its own.
// bit0 = boredom (first need). Others stay dormant at 0 until lit.
constexpr uint8_t NEEDS_ACTIVE_MASK        = 0x07;  // bit0 boredom + bit1 social + bit2 rest
// Boredom (stimulation): rises while idle/rote, drains while playing. The
// personality drive (tempo+exploration+social) scales the rise, so a busy
// curious social conker reaches full in ~12-15 min while a placid loner
// barely climbs in an hour. Average personality ≈ full in ~25 min.
constexpr float BOREDOM_RISE_PER_SEC       = 1.0f / 1000.0f;  // v158: ~17min uninterrupted to full
constexpr float BOREDOM_NIGHT_DECAY_PER_SEC = 1.0f / 600.0f;  // winds down by dusk/night
                                                             // so it never competes with sleep
constexpr float BOREDOM_PLAY_DRAIN_PER_SEC = 0.02f;  // v158: a play bout is gentle RELIEF, not a reset.
                                                     // The colony plays constantly (surplus-driven), and at
                                                     // 0.06 each bout still crashed boredom to ~0 before it
                                                     // could build; at 0.02 boredom accumulates between bouts
                                                     // and climbs past the ~0.45 it needs to win the mood
                                                     // arbiter vs tiredness, so restless/bored actually show.
constexpr float BOREDOM_WORK_RISE_SCALE    = 0.4f;   // rote work is mildly stimulating
constexpr float BOREDOM_RESTLESS_AT        = 0.55f;  // legacy fixed mood thresholds — superseded
constexpr float BOREDOM_BORED_AT           = 0.85f;  // by the per-conker _boredom_act_threshold (v152)
// v152: curiosity sets WHEN a conker bothers to act on boredom (and reads bored),
// mirroring how hardiness sets the nap threshold. exploration 0 → only acts when
// very bored (HIGH); exploration 1 → twitchy, acts early (LOW).
constexpr float BOREDOM_ACT_HIGH           = 0.80f;  // incurious: lets boredom ride
constexpr float BOREDOM_ACT_LOW            = 0.20f;  // curious: acts almost immediately
constexpr float BOREDOM_PLAY_DRIVE         = 2.0f;   // boredom's weight as a play instigator
constexpr float BOREDOM_BOOP_RELIEF        = 0.5f;   // a player tap perks them up

// ---- Need #2: tiredness (NEED_REST) ----
// Rises while awake (maxed after ~16h, hardiness scaled), falls while asleep
// (a full night restores it). They settle to sleep in the dusk→dawn window
// once reasonably tired, can power-nap any time if exhausted, and once asleep
// stay down until rested — wakeable only by a player boop or colony famine.
constexpr float TIRED_RISE_PER_SEC   = 1.0f / (16.0f * 3600.0f);  // ~16h awake → maxed
constexpr float TIRED_FALL_PER_SEC   = 1.0f / (10.0f * 3600.0f);  // ~a night's sleep restores
constexpr float TIRED_SLEEP_NIGHT    = 0.45f;  // dusk→dawn: sleep once this tired
constexpr float TIRED_SLEEP_ANY      = 0.70f;  // any time of day: sleep if this tired
constexpr float TIRED_RESTED_WAKE    = 0.15f;  // deep wake: fully rested (night sleep / daytime nap)
constexpr float TIRED_MORNING_WAKE   = 0.45f;  // dawn: night-sleepers get up once rested to here (they're
                                               // near-zero by dawn after a solid night), forced up by full DAY
constexpr float TIRED_NAP_RESTORE    = 0.15f;  // a daytime nap only tops up by this much (~1.5h) then back
                                               // to work — NOT drained to empty like a night's sleep, which
                                               // used to make naps 4-7h long and clump the colony asleep mid-morning

// ---- Needs framework: arbiter + chronotype + afterglow (v127) ----
// All built; only the needs in NEEDS_ACTIVE_MASK actually drive behaviour.
//
// Salience arbiter: each active need's salience = level × weight (× context gate
// in code). The loudest becomes the conker's intent_need → mood. Rest outweighs
// boredom so a tired conker reads sleepy even when also bored (matches the old
// "tired beats bored" rule).
// Indexed by NeedDim (conker.h): [0]=boredom, [1]=social, [2]=rest. Sized as a
// literal because NeedDim isn't visible here (conker.h includes config.h).
constexpr float NEED_SALIENCE_WEIGHT[3] = {
    1.0f,   // NEED_BOREDOM
    1.0f,   // NEED_SOCIAL
    1.4f,   // NEED_REST  (wins ties)
};
// Chronotype: dozy conkers (low hardiness/tempo) nod off sooner — even by day.
// nap_threshold = TIRED_SLEEP_ANY − CHRONO_NAP_RANGE × nappiness  (→ 0.40..0.70).
constexpr float CHRONO_NAP_RANGE     = 0.30f;
// Afterglow: a satisfied beat after a need is met, before it climbs again.
constexpr int   AFTERGLOW_TICKS      = 48;     // ~6s at 8 tps (MOOD_HAPPY)

// ---- Need #3: SOCIAL (companionship) — BUILT, DORMANT (mask bit1 off) ----
// Rises while no companion is within reach (loners climb slower), drains near
// others. A lonely conker seeks the nearest companion (response = seek company).
constexpr float SOCIAL_RISE_PER_SEC   = 1.0f / 2400.0f;  // ~40 min alone → maxed
constexpr float SOCIAL_FALL_PER_SEC   = 1.0f / 300.0f;   // company soothes in ~5 min
constexpr float SOCIAL_COMPANION_DIST = 3.0f;            // cells: a neighbour this close = company
constexpr float SOCIAL_LONELY_AT      = 0.55f;           // mood: content → restless
constexpr float SOCIAL_URGENT_AT      = 0.85f;           // mood: restless → lonely
constexpr float SOCIAL_BOOP_RELIEF    = 0.4f;            // a player tap is a bit of company

// Night huddle (v150): loneliness keeps rising (gently) through a night sleep when
// a conker dozes off alone, and a friendly conker will WAKE to go find company —
// then nestle back down. Loners (social < SOCIAL_WAKE_MIN, the same bottom-~10%
// floor as proximity bonding) sleep alone all night, untroubled. The friendlier a
// conker, the LOWER its wake threshold, so butterflies rouse sooner than the
// quiet ones:  wake_at = SOCIAL_WAKE_BASE − SOCIAL_WAKE_SLOPE × social_personality
//   social 0.22 → ~0.83 (rarely),  0.5 → ~0.68,  1.0 → 0.40 (readily).
constexpr float SOCIAL_SLEEP_RISE_SCALE = 0.5f;   // loneliness climbs at half-rate while asleep
constexpr float SOCIAL_WAKE_MIN         = 0.22f;  // below this sociability, never woken by loneliness
constexpr float SOCIAL_WAKE_BASE        = 0.95f;
constexpr float SOCIAL_WAKE_SLOPE       = 0.55f;
constexpr int   SOCIAL_SEEK_TIMEOUT_TICKS = 1200; // give up groggy-seeking and resettle if no one's reachable

// ---- Critters (visiting bugs to discover) ----
// A beetle/butterfly/worm wanders in, drifts toward the colony, and the first
// conker to reach it "discovers" it — a burst of novelty that relieves boredom
// and makes a "Dahlia found a beetle" moment. Reuses the firefly drift model.
constexpr int   MAX_CRITTERS             = 2;
constexpr float CRITTER_SPAWN_CHANCE     = 0.0012f; // per tick (daytime), gated by count
constexpr int   CRITTER_TTL_MIN          = 320;     // ~40s — wanders off if undiscovered
constexpr int   CRITTER_TTL_MAX          = 720;     // ~90s
constexpr float CRITTER_SPEED_BEETLE     = 0.065f;
constexpr float CRITTER_SPEED_BUTTERFLY  = 0.13f;
constexpr float CRITTER_SPEED_WORM       = 0.045f;
constexpr float CRITTER_SPEED_MOTH       = 0.11f;
constexpr float CRITTER_SPEED_SNAIL      = 0.02f;   // takes its time (double TTL)
constexpr float CRITTER_SPEED_LADYBIRD   = 0.06f;
constexpr float CRITTER_SPEED_DRAGONFLY  = 0.22f;   // darts
// Night visitors (moths) are rarer than the daytime parade
constexpr float CRITTER_NIGHT_SPAWN_CHANCE = 0.0004f;
constexpr float CRITTER_SEEK_CONKER      = 0.06f;   // homes toward the nearest guy
                                                    // (must beat the wander jitter
                                                    // or it never reaches the cluster)
constexpr float CRITTER_FLEE_SPEED       = 0.32f;   // scurries off once found
constexpr int   CRITTER_FLEE_TICKS       = 14;      // ~2s of fleeing, then gone
constexpr float CRITTER_FIND_RADIUS      = 1.4f;    // cells — contact = discovery
constexpr float DISCOVERY_BOREDOM_RELIEF = 0.7f;    // novelty is a big relief

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
constexpr float FAMINE_BROOD_CULL_HUNGER   = 30.0f;  // queen hunger threshold for brood cull
constexpr float QUEEN_PRIORITY_HUNGER      = 60.0f;  // famine override: feed queen urgently

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
constexpr int ENTRY_HALF_W        = 1;  // entry zone is 3 cells wide (center ± 1)

constexpr int FACE_DX[FACE_COUNT] = {  0,  0, -1,  1 };
constexpr int FACE_DY[FACE_COUNT] = { -1,  1,  0,  0 };

constexpr Face FACE_OPPOSITE[FACE_COUNT] = { FACE_S, FACE_N, FACE_E, FACE_W };

constexpr int QUEEN_SPAWN_X = GRID_WIDTH / 2;   // 30
constexpr int QUEEN_SPAWN_Y = GRID_HEIGHT / 2;  // 20
constexpr int QUEEN_BODY_HALF_W = 4;            // cells — idle ants won't enter (x)
constexpr int QUEEN_BODY_HALF_H = 5;            // cells — idle ants won't enter (y)

// ---- Stack weights & capacities ----
constexpr int STACK_WEIGHT_CONKER      = 2;
constexpr int STACK_CAPACITY_CONKER    = 7;
// Legacy aliases
constexpr int STACK_WEIGHT_PIONEER     = STACK_WEIGHT_CONKER;
constexpr int STACK_WEIGHT_MINOR       = STACK_WEIGHT_CONKER;
constexpr int STACK_WEIGHT_MAJOR       = STACK_WEIGHT_CONKER;
constexpr int STACK_CAPACITY_PIONEER   = STACK_CAPACITY_CONKER;
constexpr int STACK_CAPACITY_MINOR     = STACK_CAPACITY_CONKER;
constexpr int STACK_CAPACITY_MAJOR     = STACK_CAPACITY_CONKER;
constexpr int STACK_TOPPLE_TICKS       = 12;  // wobble duration before scatter
constexpr int STACK_FALL_TICKS         = 8;   // fall-down duration after wobble
constexpr uint32_t STACK_COLLAPSE_COOLDOWN_MS = 120000; // 120s before restacking

// ---- Husk pool ----
constexpr int MAX_HUSKS = 50;

// ---- Proximity interactions ----
constexpr int   PROXIMITY_DETECTION_RADIUS = 1;
constexpr float BASE_FORAGER_FRACTION      = 0.10f;  // foraging prob at pressure 0.0
constexpr float PROXIMITY_GREETING_CHANCE  = 0.20f;
constexpr float PROXIMITY_FOOD_SHARE_CHANCE = 0.08f;
constexpr float FOOD_SHARE_RELIEF          = 25.0f; // hunger a fed nestmate loses (trophallaxis, v161)
constexpr int   GREETING_DURATION_TICKS    = 12;   // ~1.5s at 8 tps
constexpr int   FOOD_SHARE_DURATION_TICKS  = 12;   // ~1.5s at 8 tps
constexpr int   INTERACTION_COOLDOWN_TICKS = 40;    // ~5s at 8 tps

// ---- Pool limits ----
// ---- Population cap (BREEDING limit — distinct from the render array below) ----
// The colony stops HATCHING once it reaches POP_CAP_PER_MODULE living workers per
// connected module (10 solo; +10 per docked module — pooled colony-wide). Ready
// brood then lie dormant and hatch the moment a slot frees up (a worker dies, or a
// module is added). On-screen rendering stays uncapped (MAX_CONKERS, below).
constexpr int POP_CAP_PER_MODULE = 10;

constexpr int MAX_CONKERS  = 200;
constexpr int MAX_BROOD      = 100;
constexpr int MAX_FOOD_PILES = 64;

// ---- Time-warp tuning mode (serial: `warp on | warp <hours> | warp off`) ----
// Runs the sim UNTHROTTLED at a FIXED dt-per-tick equal to the 8 tps reference
// (WARP_DT), advancing the day/night clock in lockstep. A warped run is therefore
// byte-identical to a real-time 8 tps run — needs (dt-driven) and bonds/movement
// (tick-driven) keep the exact same per-sim-day relationship they have at 8 tps,
// so there is zero distortion; it just finishes in minutes instead of a real day.
// Telemetry sim-gates (samples every 10 SIM-seconds) so a warped day fills the
// CSV like a real one. NTP is suspended and brood-stage/WiFi-push clocks (millis-
// based) do NOT advance under warp — this is a needs/mood/bond tuning instrument.
constexpr float    WARP_DT             = 1.0f / 8.0f;  // sim-seconds advanced per warp tick (= 8 tps)
constexpr int      WARP_TICKS_PER_LOOP = 80;           // ~10 sim-s/loop → serial + WDT stay responsive
constexpr uint32_t WARP_RENDER_MS      = 250;          // throttle the display so SPI isn't the bottleneck
constexpr float    WARP_DEFAULT_HOURS  = 24.0f;        // `warp on` runs one full day, then auto-stops

} // namespace Cfg
