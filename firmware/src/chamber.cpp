/* Chamber — ported from sim/chamber.py. */
#include "chamber.h"
#include "rng.h"
#include <cstring>

void Chamber::init(ColonyState* col, bool with_queen) {
    colony = col;
    lil_guy_count = 0;
    brood_count = 0;
    food_pile_count = 0;
    food_delivery_signal = 0;
    cannibalism_cooldown = 0;
    home_face = -1;
    has_queen = with_queen;

    for (int f = 0; f < FACE_COUNT; f++) entries[f] = -1;

    if (with_queen) {
        queen_obj.init(Cfg::QUEEN_SPAWN_X, Cfg::QUEEN_SPAWN_Y);
        colony->food_total = Cfg::FOOD_STORE_START;
    }
}

void Chamber::tick() {
    // Pheromone decay
    pheromones.decay();

    // Queen beacon
    if (has_queen && queen_obj.alive) {
        pheromones.deposit_home(queen_obj.x, queen_obj.y,
                                Cfg::BASE_MARKER_INTENSITY);
    }

    // Queen tick
    if (has_queen) queen_obj.tick(*this);

    // Brood tick
    for (int i = brood_count - 1; i >= 0; i--) {
        bool hatch = brood[i].tick();
        if (hatch) {
            bool pioneer = colony->total_workers_born < Cfg::QUEEN_FOUNDING_EGG_CAP;
            add_lil_guy(brood[i].x, brood[i].y, brood[i].role, pioneer);
            colony->total_workers_born++;
            remove_brood(i);
        } else if (!brood[i].alive()) {
            remove_brood(i);
        }
    }

    // Starvation cull — hungry larvae under high pressure
    float pressure = colony->food_pressure();
    if (pressure > Cfg::FAMINE_BROOD_CULL_PRESSURE) {
        for (int i = brood_count - 1; i >= 0; i--) {
            if (brood[i].stage == STAGE_LARVA && brood[i].alive()
                    && brood[i].hunger > Cfg::FAMINE_BROOD_CULL_HUNGER) {
                remove_brood(i);
            }
        }
    }

    // Cooldowns
    if (cannibalism_cooldown > 0) cannibalism_cooldown--;
    if (food_delivery_signal > 0) food_delivery_signal--;

    // Shuffle workers (Fisher-Yates)
    for (int i = lil_guy_count - 1; i > 0; i--) {
        int j = g_rng.rand_int(0, i);
        if (i != j) {
            LilGuy tmp = lil_guys[i]; lil_guys[i] = lil_guys[j]; lil_guys[j] = tmp;
        }
    }

    // Save prev positions
    for (int i = 0; i < lil_guy_count; i++) {
        lil_guys[i].prev_x = lil_guys[i].x;
        lil_guys[i].prev_y = lil_guys[i].y;
    }

    // Worker ticks + edge crossing + death
    for (int i = lil_guy_count - 1; i >= 0; i--) {
        lil_guys[i].tick(*this);
        if (!lil_guys[i].alive) {
            if (lil_guys[i].food_carried > 0)
                add_food(lil_guys[i].cell_x(), lil_guys[i].cell_y(), lil_guys[i].food_carried);
            remove_lil_guy(i);
        }
        // Edge crossing check (single-board: no neighbors, so no crossings)
    }
}

// ---- food piles ----

void Chamber::add_food(int x, int y, float amount) {
    if (!in_bounds(x, y)) return;
    int idx = _food_pile_index(x, y);
    if (idx >= 0) {
        food_piles[idx].amount += amount;
        return;
    }
    if (food_pile_count >= Cfg::MAX_FOOD_PILES) return;
    food_piles[food_pile_count++] = {static_cast<int8_t>(x),
                                     static_cast<int8_t>(y), amount};
}

float Chamber::take_food(int x, int y, float amount) {
    int idx = _food_pile_index(x, y);
    if (idx < 0) return 0.0f;
    float pile = food_piles[idx].amount;
    if (pile <= 0.0f) return 0.0f;
    float taken = (amount < pile) ? amount : pile;
    food_piles[idx].amount -= taken;
    if (food_piles[idx].amount <= 0.0f) {
        // Swap-with-last removal
        food_piles[idx] = food_piles[--food_pile_count];
    }
    return taken;
}

bool Chamber::nearest_food_within(int x, int y, int radius,
                                   int8_t& out_x, int8_t& out_y) {
    int best_d = radius + 1;
    bool found = false;
    for (int i = 0; i < food_pile_count; i++) {
        if (food_piles[i].amount <= 0.0f) continue;
        int d = abs(food_piles[i].x - x) + abs(food_piles[i].y - y);
        if (d < best_d) {
            best_d = d;
            out_x = food_piles[i].x;
            out_y = food_piles[i].y;
            found = true;
        }
    }
    return found;
}

float Chamber::total_food() const {
    float total = 0.0f;
    for (int i = 0; i < food_pile_count; i++)
        total += food_piles[i].amount;
    return total;
}

int Chamber::_food_pile_index(int x, int y) const {
    for (int i = 0; i < food_pile_count; i++) {
        if (food_piles[i].x == x && food_piles[i].y == y)
            return i;
    }
    return -1;
}

// ---- pheromone deposits (with diffusion) ----

void Chamber::deposit_home(int x, int y, float amount) {
    _deposit_home_cell(x, y, amount);
    float half = amount * 0.5f;
    _deposit_home_cell(x+1, y, half);
    _deposit_home_cell(x-1, y, half);
    _deposit_home_cell(x, y+1, half);
    _deposit_home_cell(x, y-1, half);
}

void Chamber::deposit_food(int x, int y, float amount) {
    _deposit_food_cell(x, y, amount);
    float half = amount * 0.5f;
    _deposit_food_cell(x+1, y, half);
    _deposit_food_cell(x-1, y, half);
    _deposit_food_cell(x, y+1, half);
    _deposit_food_cell(x, y-1, half);
}

void Chamber::_deposit_home_cell(int x, int y, float amount) {
    pheromones.deposit_home(x, y, amount);
    // Mirror: would send to neighbor in multi-board mode
}

void Chamber::_deposit_food_cell(int x, int y, float amount) {
    pheromones.deposit_food(x, y, amount);
}

// ---- pool management ----

void Chamber::add_lil_guy(int8_t px, int8_t py, Role c, bool pioneer) {
    if (lil_guy_count >= Cfg::MAX_LIL_GUYS) return;
    lil_guys[lil_guy_count].init(px, py, c, pioneer);
    lil_guy_count++;
}

void Chamber::remove_lil_guy(int idx) {
    if (idx < 0 || idx >= lil_guy_count) return;
    lil_guys[idx] = lil_guys[--lil_guy_count];
}

void Chamber::add_brood(int8_t px, int8_t py, Role c) {
    if (brood_count >= Cfg::MAX_BROOD) return;
    brood[brood_count].init(px, py, c);
    brood_count++;
}

void Chamber::remove_brood(int idx) {
    if (idx < 0 || idx >= brood_count) return;
    brood[idx] = brood[--brood_count];
}

void Chamber::count_brood(uint16_t& eggs, uint16_t& larvae, uint16_t& pupae) const {
    eggs = larvae = pupae = 0;
    for (int i = 0; i < brood_count; i++) {
        if (!brood[i].alive()) continue;
        switch (brood[i].stage) {
            case STAGE_EGG:   eggs++;   break;
            case STAGE_LARVA: larvae++; break;
            case STAGE_PUPA:  pupae++;  break;
            default: break;
        }
    }
}

int Chamber::_entry_face_at(int x, int y) const {
    for (int f = 0; f < FACE_COUNT; f++) {
        if (Cfg::ENTRY_X[f] == x && Cfg::ENTRY_Y[f] == y)
            return f;
    }
    return -1;
}
