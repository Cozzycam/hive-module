/* Worker conker -- JohnBuffer-inspired marker-following.
 *
 * Movement engine:
 *   Workers have float (x, y) positions and move fractionally each tick
 *   toward a target_cell (int, int). Cell derivation: int(floor(x/y)).
 *   Decision handlers only fire when has_target_cell is false (arrived).
 *
 *   Pheromone deposits happen on cell entry (tracked by last_cell),
 *   not every tick, so trail density matches actual grid traversal.
 */
#pragma once
#include "config.h"
#include <cmath>

// Per-conker animation overlay (does not change state machine)
enum ConkerAnim : uint8_t {
    LG_ANIM_NONE              = 0,
    LG_ANIM_GREETING          = 1,
    LG_ANIM_FOOD_SHARE_GIVER  = 2,
    LG_ANIM_FOOD_SHARE_RECEIVER = 3,
    LG_ANIM_GROOMING            = 4,
    LG_ANIM_SNOOZE              = 5,
    LG_ANIM_TOPPLE              = 6,
    LG_ANIM_NOTICE              = 7,  // tapped: stop, face the glass, startle hop
};

// Sprite frame selection — animations can request dedicated sprite art
enum ConkerSpriteFrame : uint8_t {
    LG_FRAME_BASE     = 0,   // default sprite
    LG_FRAME_LEAN     = 1,   // hand-painted lean frame (pending)
    LG_FRAME_CARRYING = 2,   // carrying seed/larva/corpse (future)
    LG_FRAME_GROOMING = 3,   // reserved for grooming brief
    LG_FRAME_SNOOZE   = 4,   // sleeping pose
    LG_FRAME_DORMANT  = 5,   // reserved for queen hibernation
};

class Chamber;  // forward declaration

// Needs (drives). The framework is wired up whole; Cfg::NEEDS_ACTIVE_MASK
// gates which ones actually evolve so each can be tuned in isolation.
enum NeedDim : uint8_t {
    NEED_BOREDOM = 0,   // stimulation / play (first active need)
    NEED_SOCIAL  = 1,   // companionship (dormant)
    NEED_REST    = 2,   // energy (dormant)
    NEED_COUNT   = 3,
};

// Readable summary of the loudest unmet need — drives the on-screen emote
// and the app. Live states (sleeping, playing) are folded in here too.
enum ConkerMood : uint8_t {
    MOOD_CONTENT  = 0,
    MOOD_RESTLESS = 1,   // a need is rising
    MOOD_BORED    = 2,   // a need is urgent
    MOOD_PLAYING  = 3,   // actively relieving it
    MOOD_SLEEPY   = 4,   // tired — heading for bed
    MOOD_HAPPY    = 5,   // afterglow — a need was just met (active)
    MOOD_LONELY   = 6,   // social need urgent (dormant until NEED_SOCIAL lit)
    MOOD_COUNT    = 7,
};

// "What are they up to right now" — a single readable activity derived from
// state + sub-state, shared by the on-screen boop label and the app's
// character detail. ACTIVITY_KEY[] (snake_case, stable — the app maps it to
// flavour text) and ACTIVITY_SHORT[] (the terse on-glass label) are indexed by
// this and MUST stay in lockstep with it. See conker_activity().
enum ConkerActivity : uint8_t {
    ACT_IDLING = 0,
    ACT_SLEEPING,
    ACT_NAPPING,
    ACT_SEEKING_COMPANY,
    ACT_FORAGING,
    ACT_CARRYING_FOOD,
    ACT_HEADING_HOME,
    ACT_EATING,
    ACT_CHASING_FIREFLY,
    ACT_PLAYING,
    ACT_PARADING,
    ACT_SOWING,
    ACT_GARDENING,
    ACT_TO_GARDEN,
    ACT_TENDING_BROOD,
    ACT_FEEDING_QUEEN,
    ACT_CRAFTING,
    ACT_MOURNING,
    ACT_CLEARING,
    ACT_AWAY,
    ACT_COUNT,
};

// How a conker chooses to act on its loudest need. The arbiter picks the need;
// this picks the flavour of response, coloured by personality. Built whole;
// only the boredom/rest styles are reachable while those needs are the live ones.
enum ResponseStyle : uint8_t {
    RESP_NONE         = 0,
    RESP_PLAY_SOCIAL  = 1,   // bored + sociable → seek a partner, parade
    RESP_PLAY_SOLO    = 2,   // bored + loner/explorer → solo zoomie / wander
    RESP_FIDGET       = 3,   // bored + placid → twirl/potter in place
    RESP_SLEEP        = 4,   // tired → bed down (chronotype sets when)
    RESP_SEEK_COMPANY = 5,   // lonely → drift to nearest companion (dormant)
};

// Personality dimension indices
enum PersonalityDim : uint8_t {
    PERS_WORK_TEMPO = 0,
    PERS_EXPLORATION = 1,
    PERS_ROUTE_STICKINESS = 2,
    PERS_SOCIAL_FREQUENCY = 3,
    PERS_APPETITE = 4,      // (was food_preference) eats sooner; hoards vs shares food
    PERS_HARDINESS = 5,
    PERS_PLAYFULNESS = 6,   // (was learning_rate) loves play for its own sake
    PERS_BRAVERY = 7,       // (was reserve) ventures out & leads vs timid/stays-near-queen
    PERS_COUNT = 8
};

struct Conker {
    uint32_t id             = 0;           // persistent identity (0 = unassigned)
    char     name[16]       = {};          // display name (carried via handoff)
    float    personality[PERS_COUNT] = {}; // 0.0-1.0 per dimension
    float    x, y, prev_x, prev_y;        // float cell-center coords
    AntState state          = STATE_IDLE;
    int8_t   target_x, target_y;           // high-level task target (queen/brood pos)
    bool     has_target     = false;
    int8_t   target_cell_x, target_cell_y; // next cell to walk toward
    bool     has_target_cell = false;
    uint32_t born_at_ms     = 0;
    uint32_t lived_ms       = 0;           // cumulative sim-running time
    bool     alive          = true;

    // Gateway incubation / princess (tamagotchi) — only meaningful when the
    // chamber has incubation_mode = true. She never dies; neglect suspends her.
    bool     dormant        = false;       // tardigrade sleep (neglected) — suspended, not dead
    float    keeper_bond    = 0.0f;        // closeness to the keeper (0..1); grows w/ care, cools while dormant

    Role     role           = ROLE_CONKER;
    uint8_t  move_ticks;                   // kept for reference
    uint8_t  sense_radius;
    float    carry_amount;
    float    speed;                        // cells per tick
    uint32_t lifespan_ms;
    float    scale_factor   = 3.2f;        // per-conker render scale (Gaussian at spawn)
    bool     is_pioneer     = false;       // legacy (always false for new conkers)
    bool     is_founder     = false;       // true for first FOUNDER_COHORT_SIZE hatches

    float    facing_dx, facing_dy;         // normalized velocity direction
    float    last_dx, last_dy;
    float    food_carried   = 0.0f;

    uint16_t steps_walked   = 0;
    uint16_t ticks_away     = 0;
    uint16_t stall_ticks    = 0;
    uint16_t idle_cooldown  = 0;
    uint16_t chamber_steps  = 0;
    float    hunger         = 0.0f;

    // Idle/rest state
    int16_t  idle_ticks_remaining = 0;  // >0 = truly resting
    int16_t  idle_repoll_tick     = 0;  // countdown to next _pick_task poll
    uint8_t  idle_microstate      = 0;  // 0=hold, 1=drift, 2=reface
    int16_t  idle_micro_ticks     = 0;  // current microstate duration

    // Animation overlay
    uint8_t  anim_type            = LG_ANIM_NONE;
    uint16_t anim_remaining_ticks = 0;
    uint16_t interaction_cooldown = 0;
    int8_t   anim_lean_dx         = 0;   // lean direction toward partner (-1, 0, +1)
    int8_t   anim_lean_dy         = 0;

    // Stacking (greeting towers)
    int16_t  stack_on             = -1;  // index of ant below (-1 = ground)
    uint8_t  stack_hop_remaining  = 0;   // hop-on animation countdown
    int8_t   topple_depth         = 0;   // stack depth at collapse (for fall anim)
    uint32_t stack_cooldown_ms    = 0;   // can't stack again until this time

    // Sleep
    bool     sleeping             = false;
    uint32_t sleep_until_ms       = 0;   // wake time (millis)
    uint32_t sleep_cooldown_ms    = 0;   // can't sleep again until this time

    // Night huddle (v150): woke from a lonely sleep to go find company; transient,
    // not persisted/handed-off (default false = just awake).
    bool     seeking_company      = false;
    uint16_t seek_ticks           = 0;   // groggy-seek give-up countdown

    // Zoomies (daytime chase) and parades (follow-the-leader)
    int16_t  zoomie_target        = -1;  // index of conker being chased
    int16_t  zoomie_ticks         = 0;   // countdown to end
    uint8_t  zoomie_style         = 0;   // 0=chase (sprint), 1=parade (trot)

    // Farming: the sow lean is a timed animation the generic freeze handler
    // counts down (and clears anim_type on completion), so _do_farming can't
    // read anim_type to tell "mid-sow" from "done" — this flag tracks it.
    bool     sowing               = false;

    // Forage flair (cosmetic; reset on every task pick)
    uint8_t  flair_kind           = 0;   // 0=none, 1=search cast, 2=pile inspect linger
    uint8_t  flair_ticks          = 0;   // countdown while holding a flair pause
    uint8_t  flair_casts_used     = 0;   // casts this trip (capped)
    bool     flair_ceremony_done  = false;  // pickup inspection done this trip

    // Needs / mood (v114) — needs[i] in 0..1, 1 = urgent. mood is derived
    // each tick for the renderer + API. See NeedDim / ConkerMood.
    float    needs[NEED_COUNT]    = {};
    uint8_t  mood                 = MOOD_CONTENT;
    uint8_t  intent_need          = NEED_COUNT;  // loudest need this tick (arbiter); NEED_COUNT = none
    uint16_t afterglow_ticks      = 0;           // satisfied beat after a need is met
    uint8_t  catches              = 0;           // critters caught — earns the Catcher trait

    // Making (STATE_CRAFTING) + mourning context
    uint8_t  craft_kind           = 0;           // ArtKind being made
    uint8_t  craft_context        = 0;           // ArtContext at inspiration
    int16_t  craft_ticks          = 0;           // work remaining at the spot
    uint32_t craft_for            = 0;           // accessory gifts: recipient id
    uint32_t muse_rest_unix       = 0;           // the muse rests after each work
                                                 // (rides ConkerTransfer — crossings
                                                 // must not reset it, cf. catches)
    char     mourning_for[16]     = {};          // who the vigil (and any memorial) honours

    // Worn hat (0 = none, 1 = petal hat, 2 = seed cap, 3 = grass hat) —
    // a gift from a best friend, rendered in the MAKER's tint. Taken off
    // if the friendship formally breaks; kept for life (memorial) if the
    // maker died while they were still friends.
    uint8_t  accessory            = 0;
    uint8_t  accessory_tint       = 0;           // maker's tint_seed (0 = legacy/unknown)
    uint32_t accessory_from       = 0;           // maker's id (0 = legacy/unknown)
    bool     accessory_memorial   = false;       // maker died a friend — never removed
    bool     daytime_nap          = false;       // this sleep began in the day (a nap, not a night's sleep)
    float    nap_wake_target      = 0.0f;        // tiredness a daytime nap restores down to (short top-up, not drained to empty)

    uint8_t  tint_seed            = 0;   // per-worker colour variation (set at init)
    int8_t   arrival_face        = -1;  // face this ant arrived from (-1 = locally spawned)
    uint32_t arrival_ms          = 0;   // millis() when placed after transfer

    // Departing state — worker is visually gone but still owned by this module
    bool     departing           = false;
    uint32_t depart_at_ms        = 0;   // when to actually send the handoff
    int8_t   depart_face         = -1;  // which face to depart through

    int8_t   last_cell_x, last_cell_y;     // for pheromone deposit gating

    // Helper: current grid cell from float position
    int cell_x() const { return static_cast<int>(floorf(x)); }
    int cell_y() const { return static_cast<int>(floorf(y)); }

    // Elder: past 70% of lifespan — huddle magnet, gets groomed, never climbed on
    bool is_elder() const {
        return lifespan_ms > 0 && lived_ms >= static_cast<uint32_t>(lifespan_ms * 0.7f);
    }

    // Twilight: the final stretch (last ~10% of life) — they slow down,
    // and friends are drawn to keep them company
    bool is_twilight() const {
        return lifespan_ms > 0 && lived_ms >= static_cast<uint32_t>(lifespan_ms * 0.9f);
    }

    // Role speed, tempered by age — twilight conkers walk shorter, slower loops
    float base_speed() const {
        float s = Cfg::ROLE_PARAMS[role].speed;
        return is_twilight() ? s * 0.65f : s;
    }

    // Making aptitude — the colony's artists: playful imagination with a
    // wandering eye. Emergent from personality, no new dims.
    float muse() const {
        return 0.6f * personality[PERS_PLAYFULNESS]
             + 0.4f * personality[PERS_EXPLORATION];
    }

    // Chronotype: the curious, unhurried ones keep late hours — pottering
    // and firefly-watching after the others bed down, lying in once the
    // sun is up. ~1 in 5 conkers. Emergent from personality, no new dims.
    bool is_night_owl() const {
        return personality[PERS_EXPLORATION] >= 0.6f
            && personality[PERS_WORK_TEMPO] <= 0.5f;
    }

    // Farming aptitude — emergent from personality, no new dims: steady
    // workers who love routine and stay near home make natural gardeners.
    float green_thumb() const {
        return 0.5f * personality[PERS_WORK_TEMPO]
             + 0.3f * personality[PERS_ROUTE_STICKINESS]
             + 0.2f * (1.0f - personality[PERS_EXPLORATION]);
    }

    // Hatchlings grow into their full size over their first day — visible
    // "they grow up" progress on the glass. Render-only: sim logic
    // (collision, movement) keeps using scale_factor.
    float render_scale() const {
        if (lived_ms >= Cfg::GROWTH_JUVENILE_MS) return scale_factor;
        return scale_factor * (0.6f + 0.4f * static_cast<float>(lived_ms)
                                            / Cfg::GROWTH_JUVENILE_MS);
    }

    void init(int8_t px, int8_t py, Role c = ROLE_CONKER, bool pioneer = false);
    void tick(Chamber& chamber, float dt);

    // Movement methods
    bool _set_target_cell(int cx, int cy, Chamber& ch);
    void _advance_toward_target(Chamber& ch);
    void _on_enter_cell(int cx, int cy, Chamber& ch);

    // Behavior methods
    void _pick_task(Chamber& ch);
    void _do_to_food(Chamber& ch);
    void _do_to_home(Chamber& ch);
    void _do_tend_brood(Chamber& ch);
    void _do_tend_queen(Chamber& ch);
    void _do_idle(Chamber& ch);
    void _update_needs(Chamber& ch, float dt);
    // Needs arbiter helpers (v127 framework)
    float   _need_salience(uint8_t need, Chamber& ch) const;  // context-gated salience
    float   _nap_threshold() const;          // chronotype: dozy ones nap sooner (even by day)
    float   _boredom_act_threshold() const;  // curiosity: curious ones act on boredom sooner (v152)
    float   _play_desire(Chamber& ch) const; // unified driven play urge (boredom×playfulness + surplus)
    float   _social_desire(Chamber& ch) const; // unified driven social urge (sociability + loneliness need)
    bool    _should_wake(Chamber& ch) const; // night sleeps hold to morning; naps are a short top-up
    bool    _wants_company_wake(Chamber& ch) const; // v150: friendly + lonely + alone → rouse to go huddle
    bool    _wants_company_awake(Chamber& ch) const; // v163: awake + lonely + alone → amble over to company
    void    _tick_seek_company(Chamber& ch);        // v150: groggy drift to nearest friend, then resettle
    uint8_t _response_style(uint8_t need) const;  // personality-flavoured response to a need
    float   _companions_near(Chamber& ch, bool include_sleeping = false) const; // company score within SOCIAL_COMPANION_DIST (friends weigh more)
    bool    _is_friend(Chamber& ch, uint32_t other_id) const;       // I've bonded to them (one-way ok)
    bool    _is_best_friend(Chamber& ch, uint32_t other_id) const;  // bond is mutual
    bool    _forage_follow_friend(Chamber& ch);   // tag along with a foraging friend (shared trails)
    void _tick_idle(Chamber& ch);
    void _pick_idle_microstate(Chamber& ch);
    float _colony_idle_budget(Chamber& ch);
    void _do_eating(Chamber& ch);
    void _do_cannibalize(Chamber& ch);
    void _do_zoomies(Chamber& ch);
    void _do_mourning(Chamber& ch);
    void _do_farming(Chamber& ch);
    void _do_to_garden(Chamber& ch);
    void _do_crafting(Chamber& ch);
    void _rest_muse();
    bool _start_crafting(Chamber& ch, uint8_t kind, uint8_t context,
                         int near_x, int near_y);  // find a spot, set out
    bool _target_still_valid(Chamber& ch);

    // Marker sampling -- returns true and sets out_dx/out_dy if gradient found
    bool _sample_markers(Chamber& ch, bool use_food, int8_t& out_dx, int8_t& out_dy);

    void _step_toward_cell(int tx, int ty, Chamber& ch);
    void _persistent_forward_step(Chamber& ch);
    void _explore_or_wander(Chamber& ch);
    void _explore_with_flair(Chamber& ch);  // blind exploration + chance of a search cast
    bool _flair_allowed(Chamber& ch);
    int  _max_foragers(Chamber& ch);  // forager cap incl. waggle recruitment boost

    bool _within_sense(int tx, int ty) const {
        int cx = cell_x(), cy = cell_y();
        int d = abs(tx - cx) + abs(ty - cy);
        return d <= sense_radius;
    }
    bool _detects_food_trail(Chamber& ch);
    int  _nearest_hungry_larva(Chamber& ch);   // returns brood index, -1 if none
    int  _least_invested_larva(Chamber& ch);   // returns brood index, -1 if none
    bool _food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y);
    bool _nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
                               int8_t& out_x, int8_t& out_y);
};

// Current activity of a live conker (needs the chamber for the garden-post
// check). Callers: the on-screen boop label and the /lilguys API.
ConkerActivity conker_activity(const Conker& w, const Chamber& ch);
const char* conker_activity_key(ConkerActivity a);    // stable snake_case (API)
const char* conker_activity_short(ConkerActivity a);  // terse label (on glass)
