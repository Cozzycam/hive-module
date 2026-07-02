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
    CRITTER_KIND_COUNT = 4,
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

class Chamber {
public:
    ColonyState* colony;
    BondStore*   bonds = nullptr;   // friendships (owned by Coordinator); may be null on satellites
    PheromoneGrid pheromones;
    EventBus*    event_bus = nullptr;   // transient, set per-tick by Sim
    uint32_t     tick_num  = 0;         // transient, set per-tick by Sim
    bool         is_garden = false;     // garden role: more critters, wild sprouts
    bool         gather_active = false; // transient: finger held, conkers rush here
    bool         gather_is_exit = false; // true = heading to edge to cross modules
    float        gather_x = 0, gather_y = 0; // cell coords

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
