/* Chamber — one module's local sim space. Ported from sim/chamber.py. */
#pragma once
#include "config.h"
#include "colony_state.h"
#include "pheromone_grid.h"
#include "brood.h"
#include "conker.h"
#include "queen.h"
#include "events.h"

class BondStore;  // friendships — set by Coordinator so behaviour can read them

struct FoodPile {
    int8_t x, y;
    float  amount;
};

struct Husk {
    int8_t   x, y;
    uint32_t conker_id;
    uint32_t died_unix;
    float    scale_factor;
};

// Night ambience: drifting glow-bugs that conkers occasionally chase
struct Firefly {
    float   x = 0, y = 0, prev_x = 0, prev_y = 0;  // cell coords
    float   vx = 0, vy = 0;
    uint8_t glow_phase = 0;   // wraps; drives the blink pulse
    bool    active = false;
};

// Visiting critters — a beetle/butterfly/worm wanders in for a conker to find
enum CritterKind : uint8_t {
    CRITTER_BEETLE     = 0,
    CRITTER_BUTTERFLY  = 1,
    CRITTER_WORM       = 2,
    CRITTER_FIREFLY    = 3,   // the night critter — caught via the firefly chase
    // Variant visitors (v172) — rarer, condition-gated collection fodder
    CRITTER_MOTH       = 4,   // clear nights only
    CRITTER_SNAIL      = 5,   // rain redirects the guest list to snails
    CRITTER_LADYBIRD   = 6,   // rare lucky visitor on fair days
    CRITTER_DRAGONFLY  = 7,   // garden-only prize on warm clear days
    CRITTER_KIND_COUNT = 8,
};

struct Critter {
    float    x = 0, y = 0, prev_x = 0, prev_y = 0;  // cell coords
    float    vx = 0, vy = 0;
    uint8_t  kind = 0;          // CritterKind
    uint16_t ttl = 0;           // ticks before it wanders off on its own
    uint8_t  flee = 0;          // >0: found — scurries off, then despawns
    uint8_t  anim_phase = 0;    // wing flap / wriggle cycle
    bool     active = false;
};

// ---- Making (artifacts) ----
// Conkers author the world: sculptures, cairns, paintings, memorials.
// Each work carries its maker's identity — rendered in their tint, with
// the moment of its making (storm, plenty, grief...) stored as provenance.
enum ArtKind : uint8_t {
    ART_SCULPTURE = 0,
    ART_CAIRN     = 1,
    ART_PAINTING  = 2,
    ART_MEMORIAL  = 3,   // carved by a grieving friend, names the departed
    // Accessories — worn, not placed; made FOR a best friend (honoree =
    // the recipient). Never passed to place_artwork. All three are HATS
    // since v212 (pendant/sash drowned in the wearer's body tint; hats sit
    // above the head against the floor and always read). Enum names kept
    // for wire/persistence stability.
    ART_HAT       = 4,   // petal hat
    ART_PENDANT   = 5,   // seed cap
    ART_BAND      = 6,   // grass hat
    ART_KIND_COUNT
};

enum ArtContext : uint8_t {
    CTX_PLENTY   = 0,   // a time of plenty
    CTX_STORM    = 1,   // as a storm battered the colony
    CTX_HEATWAVE = 2,   // in a scorching spell
    CTX_RAIN     = 3,   // on a rain-soaked day
    CTX_NIGHT    = 4,   // by firefly light
    CTX_GRIEF    = 5,   // in memory of someone
};

struct Artwork {
    bool     active = false;
    uint8_t  kind = 0;           // ArtKind
    int8_t   x = 0, y = 0;
    uint32_t maker_id = 0;
    char     maker_name[16] = {};
    uint8_t  maker_tint = 0;     // rendered in the maker's own hue
    uint32_t created_unix = 0;
    uint8_t  context = 0;        // ArtContext at the moment of making
    uint8_t  motif = 0;          // procedural look variant
    char     honoree[16] = {};   // memorials: who it remembers
    uint16_t admired = 0;        // times a passerby stopped for it
    uint32_t last_visited_unix = 0;  // memorials: last anniversary visit
};

// ---- Garden farming ----
// Crops grow on WALL-CLOCK time (the lifecycle clock): sown by a
// green-thumbed conker, they pass sprout → growing → mature, then drop a
// food pile the ordinary foraging machinery carries home.
enum PlantStage : uint8_t {
    PLOT_SOIL    = 0,   // empty, sowable
    PLOT_SPROUT  = 1,
    PLOT_GROWING = 2,
    PLOT_MATURE  = 3,   // drops its yield on the next plant tick
};

struct Plant {
    uint8_t  stage = PLOT_SOIL;
    int8_t   x = 0, y = 0;          // fixed plot cell
    uint32_t stage_started_unix = 0;
    uint32_t sown_by = 0;           // conker id, for the chronicle's credit
    char     sown_by_name[16] = {};
};

class Chamber {
public:
    ColonyState* colony;
    BondStore*   bonds = nullptr;   // friendships (owned by Coordinator); may be null on satellites
    PheromoneGrid pheromones;
    EventBus*    event_bus = nullptr;   // transient, set per-tick by Sim
    uint32_t     tick_num  = 0;         // transient, set per-tick by Sim
    bool         is_garden = false;     // garden role: more critters, wild sprouts
    uint8_t      queen_tint = 0;        // imported (app-raised) queen wears her own colour; 0 = default palette
    bool         incubation_mode = false; // Gateway tamagotchi: a single raised "princess" conker —
                                          // never dies (dormant on neglect), no queen/founding. OFF on hardware.
    // Followed conkers (app pins) — the renderer stars them on the glass
    static constexpr int MAX_FOLLOWED = 8;
    uint32_t     followed[MAX_FOLLOWED] = {};
    uint8_t      followed_count = 0;
    bool is_followed(uint32_t id) const {
        for (int i = 0; i < followed_count; i++)
            if (followed[i] == id) return true;
        return false;
    }
    bool         gather_active = false; // transient: finger held, conkers rush here
    bool         gather_is_exit = false; // true = heading to edge to cross modules
    float        gather_x = 0, gather_y = 0; // cell coords

    // The keeper's ball (incubation only). One at a time: she chases it, bats it
    // somewhere new, chases again, until it rolls to a stop and you throw again.
    // Deliberately not an entity system — a one-creature pet needs one toy.
    // Cell coords, but FLOAT and with velocity — the ball rolls and slows rather
    // than teleporting between cells (it used to blink, which read as a glitch
    // instead of a knock-on). Same shape as Critter/Firefly so the renderer can
    // sub-tick lerp it the same way.
    float        ball_x = 0, ball_y = 0;
    float        ball_prev_x = 0, ball_prev_y = 0;
    float        ball_vx = 0, ball_vy = 0;
    float        ball_roll = 0;              // accumulated spin phase, drives the highlight
    bool         ball_active = false;
    uint8_t      ball_bounces = 0;           // bats left before it rolls to a stop for good
    uint8_t      ball_bat_cooldown = 0;      // ticks before she can bat it again
    bool has_ball() const { return ball_active && ball_bounces > 0; }
    bool ball_resting() const;               // slow enough to pounce on
    void throw_ball(int x, int y);           // keeper throws (resets the bounce count)
    void bat_ball(float from_x, float from_y);  // she reaches it: knock it on, away from her
    void _tick_ball();                       // roll + friction + wall bounce

    Queen  queen_obj;
    bool   has_queen = false;

    Conker conkers[Cfg::MAX_CONKERS];
    int    conker_count = 0;

    Brood  brood[Cfg::MAX_BROOD];
    int    brood_count = 0;

    FoodPile food_piles[Cfg::MAX_FOOD_PILES];
    int      food_pile_count = 0;

    Husk     husks[Cfg::MAX_HUSKS];
    int      husk_count = 0;

    Firefly  fireflies[Cfg::MAX_FIREFLIES];
    // Nearest active firefly within manhattan radius, -1 if none
    int  nearest_firefly(int cx, int cy, int radius) const;
    void _tick_fireflies();

    Critter  critters[Cfg::MAX_CRITTERS];
    void _tick_critters();
    void _detect_critter_discovery();   // contact → discovery event + boredom relief

    // Garden farming (only ticks when is_garden)
    Plant plants[Cfg::GARDEN_PLOTS];
    void _tick_plants();
    int  free_plot() const;             // -1 if none sowable
    bool sow_plot(int idx, uint32_t by_id, const char* by_name);

    // The garden post: one green thumb holds it and stays ready to work the
    // plots; the rest of the colony passes through as usual. Vacancy (holder
    // left/died/disqualified) is advertised via pop sync so the queen can
    // send a replacement. Not persisted — re-claimed within ~1s of any boot.
    uint32_t posted_gardener = 0;       // conker id, 0 = vacant
    bool garden_post_filled();          // holder still present, alive, qualified
    bool garden_post_claim(uint32_t conker_id);   // hold or take the post
    void garden_post_release(uint32_t conker_id); // step down (if holder)

    // Making — artifacts placed by conkers (any module)
    Artwork artworks[Cfg::MAX_ARTWORKS];
    // Places a work (evicting the oldest if full — the eviction is the
    // caller's story to tell via the returned index). Returns slot or -1.
    int  place_artwork(const Artwork& piece, Artwork* weathered_out);
    bool artwork_spot_free(int cx, int cy) const;   // clear of plots/food/queen/art
    int  nearest_artwork(int cx, int cy, int radius) const;  // -1 if none
    void artwork_admired(int idx);                  // count + occasional persist
    // Keeper cleanup (serial `art weather`): weathers the n least-admired
    // works (over-cap makers' surplus first) through the normal event path
    // so the diary and gallery stay truthful. Memorials are never swept.
    int  weather_artworks(int n);

    // Finger scent shimmer — visual echo of a swipe, fades over TTL
    struct ScentMark { int8_t x, y; uint8_t ttl; };
    ScentMark scent_marks[Cfg::MAX_SCENT_MARKS];
    void add_scent_mark(int cx, int cy);

    int8_t  entries[FACE_COUNT];    // neighbor ID per face, -1 = none
    int8_t  home_face = -1;         // face toward queen chamber, -1 = is queen chamber

    uint8_t food_delivery_signal = 0;
    int     cannibalism_cooldown = 0;

    // Lifecycle notifications — populated during tick(), drained by Coordinator
    static constexpr int MAX_LIFECYCLE = 16;
    struct DeathNotice { uint32_t id; uint8_t cause; };  // 0=old_age, 1=starved
    DeathNotice deaths[MAX_LIFECYCLE];
    int      death_count = 0;
    uint32_t hatch_ids[MAX_LIFECYCLE];         // IDs of brood that hatched into workers
    uint32_t hatch_tended_by[MAX_LIFECYCLE];  // carer IDs for personality inheritance
    int      hatch_count = 0;

    void init(ColonyState* col, bool with_queen);
    void tick(float dt);

    bool in_bounds(int x, int y) const {
        return x >= 0 && x < Cfg::GRID_WIDTH && y >= 0 && y < Cfg::GRID_HEIGHT;
    }

    // The liveable area. The whole grid normally; in incubation, her room — the
    // ONE definition of "where things can be", so conkers, food, the ball,
    // critters and fireflies all agree. Get this wrong in one place and life
    // happens off-screen where she can't reach it.
    int room_x0() const { return incubation_mode ? Cfg::ROOM_X0 : 0; }
    int room_y0() const { return incubation_mode ? Cfg::ROOM_Y0 : 0; }
    int room_x1() const { return incubation_mode ? Cfg::ROOM_X0 + Cfg::ROOM_W_CELLS - 1 : Cfg::GRID_WIDTH  - 1; }
    int room_y1() const { return incubation_mode ? Cfg::ROOM_Y0 + Cfg::ROOM_H_CELLS - 1 : Cfg::GRID_HEIGHT - 1; }
    float clamp_room_x(float v) const {
        float lo = (float)room_x0() + 0.5f, hi = (float)room_x1() + 0.5f;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    float clamp_room_y(float v) const {
        float lo = (float)room_y0() + 0.5f, hi = (float)room_y1() + 0.5f;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // ---- events ----
    void emit(const Event& ev) {
        if (event_bus) event_bus->emit(ev);
    }

    // ---- food ----
    void  add_food(int x, int y, float amount);
    float take_food(int x, int y, float amount);
    bool  nearest_food_within(int x, int y, int radius, int8_t& out_x, int8_t& out_y);
    float total_food() const;
    // Where workers go to eat from food_store (queen now, granary later)
    void food_store_pos(int8_t& out_x, int8_t& out_y) const {
        out_x = queen_obj.x; out_y = queen_obj.y;
    }

    // ---- pheromone deposits (with diffusion + mirror) ----
    void deposit_home(int x, int y, float amount);
    void deposit_food(int x, int y, float amount);

    // ---- brood/conker pool management ----
    void add_conker(int8_t px, int8_t py, Role c = ROLE_CONKER, bool pioneer = false);
    void remove_conker(int idx);
    void add_brood(int8_t px, int8_t py, Role c = ROLE_CONKER);
    void add_brood_with_duration(int8_t px, int8_t py, uint32_t duration_ms);
    void remove_brood(int idx);
    void add_husk(int8_t px, int8_t py, uint32_t conker_id, float scale, uint32_t died_unix);

    // ---- brood counts ----
    void count_brood(uint16_t& eggs, uint16_t& seeds) const;

    // Used by Conker for food pile checks
    int  _food_pile_index(int x, int y) const;

    // Used by Coordinator for edge crossing detection
    int  _entry_face_at(int x, int y) const;  // returns Face or -1

private:
    void _deposit_home_cell(int x, int y, float amount);
    void _deposit_food_cell(int x, int y, float amount);
    void _detect_proximity_interactions();
};
