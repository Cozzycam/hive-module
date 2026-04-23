/* Chamber — one module's local sim space. Ported from sim/chamber.py. */
#pragma once
#include "config.h"
#include "colony_state.h"
#include "pheromone_grid.h"
#include "brood.h"
#include "lil_guy.h"
#include "queen.h"
#include "events.h"

struct FoodPile {
    int8_t x, y;
    float  amount;
};

class Chamber {
public:
    ColonyState* colony;
    PheromoneGrid pheromones;
    EventBus*    event_bus = nullptr;   // transient, set per-tick by Sim
    uint32_t     tick_num  = 0;         // transient, set per-tick by Sim

    Queen  queen_obj;
    bool   has_queen = false;

    LilGuy lil_guys[Cfg::MAX_LIL_GUYS];
    int    lil_guy_count = 0;

    Brood  brood[Cfg::MAX_BROOD];
    int    brood_count = 0;

    FoodPile food_piles[Cfg::MAX_FOOD_PILES];
    int      food_pile_count = 0;

    int8_t  entries[FACE_COUNT];    // neighbor ID per face, -1 = none
    int8_t  home_face = -1;         // face toward queen chamber, -1 = is queen chamber

    uint8_t food_delivery_signal = 0;
    int     cannibalism_cooldown = 0;

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

    // ---- pheromone deposits (with diffusion + mirror) ----
    void deposit_home(int x, int y, float amount);
    void deposit_food(int x, int y, float amount);

    // ---- brood/lil_guy pool management ----
    void add_lil_guy(int8_t px, int8_t py, Role c, bool pioneer);
    void remove_lil_guy(int idx);
    void add_brood(int8_t px, int8_t py, Role c);
    void remove_brood(int idx);

    // ---- brood counts ----
    void count_brood(uint16_t& eggs, uint16_t& larvae, uint16_t& pupae) const;

    // Used by LilGuy for food pile checks
    int  _food_pile_index(int x, int y) const;

private:
    void _deposit_home_cell(int x, int y, float amount);
    void _deposit_food_cell(int x, int y, float amount);
    int  _entry_face_at(int x, int y) const;  // returns Face or -1
    void _detect_proximity_interactions();
};
