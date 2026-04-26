/* Chamber — ported from sim/chamber.py. */
#include "chamber.h"
#include "rng.h"
#include <Arduino.h>
#include <cstring>
#include <cmath>

void Chamber::init(ColonyState* col, bool with_queen) {
    colony = col;
    lil_guy_count = 0;
    brood_count = 0;
    food_pile_count = 0;
    food_delivery_signal = 0;
    cannibalism_cooldown = 0;
    home_face = -1;
    has_queen = with_queen;
    event_bus = nullptr;
    tick_num = 0;

    for (int f = 0; f < FACE_COUNT; f++) entries[f] = -1;

    if (with_queen) {
        queen_obj.init(Cfg::QUEEN_SPAWN_X, Cfg::QUEEN_SPAWN_Y);
        colony->food_total = Cfg::FOOD_STORE_START;
    }
}

void Chamber::tick(float dt) {
    // Pheromone decay
    pheromones.decay();

    // Queen beacon
    if (has_queen && queen_obj.alive) {
        pheromones.deposit_home(queen_obj.x, queen_obj.y,
                                Cfg::BASE_MARKER_INTENSITY);
    }

    // Queen tick
    if (has_queen) queen_obj.tick(*this, dt);

    // Brood tick — with transition events
    for (int i = brood_count - 1; i >= 0; i--) {
        BroodTransition result = brood[i].tick(dt);
        switch (result) {
        case BROOD_HATCH: {
            bool pioneer = colony->total_workers_born < Cfg::FOUNDING_EGG_COUNT;
            int8_t bx = brood[i].x, by = brood[i].y;
            add_lil_guy(bx, by, brood[i].role, pioneer);
            colony->total_workers_born++;
            if (!queen_obj.founding_done) queen_obj.founding_done = true;
            Event ev; ev.type = EVT_YOUNG_HATCHED; ev.tick = tick_num;
            ev.young_hatched = {STAGE_PUPA, 0xFF, bx, by};
            emit(ev);
            remove_brood(i);
            break;
        }
        case BROOD_EGG_TO_LARVA: {
            Event ev; ev.type = EVT_YOUNG_HATCHED; ev.tick = tick_num;
            ev.young_hatched = {STAGE_EGG, STAGE_LARVA, brood[i].x, brood[i].y};
            emit(ev);
            break;
        }
        case BROOD_LARVA_TO_PUPA: {
            Event ev; ev.type = EVT_YOUNG_HATCHED; ev.tick = tick_num;
            ev.young_hatched = {STAGE_LARVA, STAGE_PUPA, brood[i].x, brood[i].y};
            emit(ev);
            break;
        }
        case BROOD_DIED: {
            Event ev; ev.type = EVT_YOUNG_DIED; ev.tick = tick_num;
            ev.position = {brood[i].x, brood[i].y};
            emit(ev);
            remove_brood(i);
            break;
        }
        default:
            if (!brood[i].alive()) {
                Event ev; ev.type = EVT_YOUNG_DIED; ev.tick = tick_num;
                ev.position = {brood[i].x, brood[i].y};
                emit(ev);
                remove_brood(i);
            }
            break;
        }
    }

    // Starvation cull — hungry larvae under high pressure
    float pressure = colony->food_pressure();
    if (pressure > Cfg::FAMINE_BROOD_CULL_PRESSURE) {
        for (int i = brood_count - 1; i >= 0; i--) {
            if (brood[i].stage == STAGE_LARVA && brood[i].alive()
                    && brood[i].hunger > Cfg::FAMINE_BROOD_CULL_HUNGER) {
                Event ev; ev.type = EVT_YOUNG_DIED; ev.tick = tick_num;
                ev.position = {brood[i].x, brood[i].y};
                emit(ev);
                remove_brood(i);
            }
        }
    }

    // Cooldowns
    if (cannibalism_cooldown > 0) cannibalism_cooldown--;
    if (food_delivery_signal > 0) food_delivery_signal--;

    // Shuffle workers (Fisher-Yates), fixing stack_on references
    for (int i = lil_guy_count - 1; i > 0; i--) {
        int j = g_rng.rand_int(0, i);
        if (i != j) {
            LilGuy tmp = lil_guys[i]; lil_guys[i] = lil_guys[j]; lil_guys[j] = tmp;
            // Patch any stack_on references that pointed to i or j
            for (int k = 0; k < lil_guy_count; k++) {
                if (lil_guys[k].stack_on == i)      lil_guys[k].stack_on = j;
                else if (lil_guys[k].stack_on == j) lil_guys[k].stack_on = i;
            }
        }
    }

    // Save prev positions
    for (int i = 0; i < lil_guy_count; i++) {
        lil_guys[i].prev_x = lil_guys[i].x;
        lil_guys[i].prev_y = lil_guys[i].y;
    }

    // Worker ticks + death
    for (int i = lil_guy_count - 1; i >= 0; i--) {
        lil_guys[i].tick(*this, dt);
        if (!lil_guys[i].alive) {
            if (lil_guys[i].food_carried > 0)
                add_food(lil_guys[i].cell_x(), lil_guys[i].cell_y(),
                         lil_guys[i].food_carried);
            Event ev; ev.type = EVT_LIL_GUY_DIED; ev.tick = tick_num;
            emit(ev);
            remove_lil_guy(i);
        }
    }

    // Proximity interactions between lil guys
    _detect_proximity_interactions();

    // Validate stacks: off-screen collapse
    for (int i = 0; i < lil_guy_count; i++) {
        if (lil_guys[i].stack_on < 0) continue;
        // Compute approximate screen Y with stack offset
        float screen_y = lil_guys[i].y * Cfg::CELL_SIZE;
        int cur = lil_guys[i].stack_on;
        while (cur >= 0) {
            float s = (lil_guys[cur].role == ROLE_MAJOR) ? 4.6f
                    : (lil_guys[cur].is_pioneer ? 2.3f : 3.2f);
            screen_y -= static_cast<int>(10.0f * s + 0.5f) * 0.75f;
            cur = lil_guys[cur].stack_on;
        }
        if (screen_y < 28.0f) {  // HUD_STRIP_H
            lil_guys[i].stack_on = -1;
        }
    }
}

// ---- proximity interactions ----

void Chamber::_detect_proximity_interactions() {
    if (!event_bus || lil_guy_count < 2) return;

    // Precompute which ants are part of a stack (rider or base)
    static bool in_stack[Cfg::MAX_LIL_GUYS];
    memset(in_stack, 0, sizeof(in_stack));
    for (int i = 0; i < lil_guy_count; i++) {
        if (lil_guys[i].stack_on >= 0) {
            in_stack[i] = true;                    // rider
            in_stack[lil_guys[i].stack_on] = true; // base
        }
    }

    // Simple spatial hash: cell -> first worker index, chain via -1 sentinel.
    // For bounded memory, use a fixed grid array.
    static int16_t grid_head[Cfg::GRID_CELLS];
    static int16_t grid_next[Cfg::MAX_LIL_GUYS];
    memset(grid_head, -1, sizeof(grid_head));

    for (int i = 0; i < lil_guy_count; i++) {
        if (!lil_guys[i].alive) continue;
        int cx = lil_guys[i].cell_x();
        int cy = lil_guys[i].cell_y();
        if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT)
            continue;
        int idx = cy * Cfg::GRID_WIDTH + cx;
        grid_next[i] = grid_head[idx];
        grid_head[idx] = i;
    }

    // Check same-cell and adjacent-cell pairs
    for (int cy = 0; cy < Cfg::GRID_HEIGHT; cy++) {
        for (int cx = 0; cx < Cfg::GRID_WIDTH; cx++) {
            int idx = cy * Cfg::GRID_WIDTH + cx;
            if (grid_head[idx] < 0) continue;

            // Helper: try interaction between a pair
            auto try_interact = [&](int ai, int bi) {
                auto& a = lil_guys[ai];
                auto& b = lil_guys[bi];

                // Trail protection: don't interrupt foraging workers
                bool a_on_job = (a.state == STATE_TO_FOOD)
                             || (a.state == STATE_TO_HOME && a.food_carried > 0);
                bool b_on_job = (b.state == STATE_TO_FOOD)
                             || (b.state == STATE_TO_HOME && b.food_carried > 0);
                if (a_on_job || b_on_job) return;

                // Cooldown gating
                if (a.interaction_cooldown > 0 || b.interaction_cooldown > 0) return;

                // Already animating or part of a stack
                if (a.anim_remaining_ticks > 0 || b.anim_remaining_ticks > 0) return;
                if (in_stack[ai] || in_stack[bi]) return;

                // Food sharing
                if ((a.food_carried > 0) != (b.food_carried > 0)) {
                    if (g_rng.rand_float() < Cfg::PROXIMITY_FOOD_SHARE_CHANCE) {
                        uint16_t pid = event_bus->next_pair_id();
                        Event es; es.type = EVT_INTERACTION_STARTED; es.tick = tick_num;
                        es.interaction_started = {pid, INTERACT_FOOD_SHARING,
                            static_cast<uint8_t>(Cfg::FOOD_SHARE_DURATION_TICKS)};
                        emit(es);
                        Event ee; ee.type = EVT_INTERACTION_ENDED; ee.tick = tick_num;
                        ee.interaction_ended = {pid};
                        emit(ee);

                        bool a_gives = (a.food_carried > 0);
                        a.anim_type = a_gives ? LG_ANIM_FOOD_SHARE_GIVER : LG_ANIM_FOOD_SHARE_RECEIVER;
                        b.anim_type = a_gives ? LG_ANIM_FOOD_SHARE_RECEIVER : LG_ANIM_FOOD_SHARE_GIVER;
                        a.anim_remaining_ticks = Cfg::FOOD_SHARE_DURATION_TICKS;
                        b.anim_remaining_ticks = Cfg::FOOD_SHARE_DURATION_TICKS;
                        a.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                        b.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                        return;
                    }
                }

                // Greeting → mutual grooming or stacking
                if (g_rng.rand_float() < Cfg::PROXIMITY_GREETING_CHANCE) {
                    if (g_rng.rand_float() < 0.5f) {
                        // Mutual grooming: both lean toward each other
                        a.anim_type = LG_ANIM_GROOMING;
                        b.anim_type = LG_ANIM_GROOMING;
                        a.anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
                        b.anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
                        a.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                        b.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                        float dx = b.x - a.x, dy = b.y - a.y;
                        if (fabsf(dx) >= fabsf(dy)) {
                            a.anim_lean_dx = (dx > 0) ? 1 : -1; a.anim_lean_dy = 0;
                        } else {
                            a.anim_lean_dx = 0; a.anim_lean_dy = (dy > 0) ? 1 : -1;
                        }
                        b.anim_lean_dx = -a.anim_lean_dx;
                        b.anim_lean_dy = -a.anim_lean_dy;
                        return;
                    }

                    // Stacking: one hops on top of the other
                    uint16_t pid = event_bus->next_pair_id();
                    Event es; es.type = EVT_INTERACTION_STARTED; es.tick = tick_num;
                    es.interaction_started = {pid, INTERACT_GREETING,
                        static_cast<uint8_t>(Cfg::GREETING_DURATION_TICKS)};
                    emit(es);
                    Event ee; ee.type = EVT_INTERACTION_ENDED; ee.tick = tick_num;
                    ee.interaction_ended = {pid};
                    emit(ee);

                    // Find the topmost ant in a tower
                    auto top_of = [&](int idx) -> int {
                        for (;;) {
                            int found = -1;
                            for (int j = 0; j < lil_guy_count; j++)
                                if (lil_guys[j].alive && lil_guys[j].stack_on == idx)
                                    { found = j; break; }
                            if (found < 0) return idx;
                            idx = found;
                        }
                    };

                    // Pick who becomes the base, who hops on
                    int base_i, hopper_i;
                    if (g_rng.rand_float() < 0.5f) {
                        base_i = ai; hopper_i = bi;
                    } else {
                        base_i = bi; hopper_i = ai;
                    }
                    int mount_on = top_of(base_i);

                    // Walk to ground to find bottom ant and stack depth
                    int depth = 1;
                    bool has_pioneer = lil_guys[mount_on].is_pioneer;
                    int ground = mount_on;
                    int cur = lil_guys[mount_on].stack_on;
                    while (cur >= 0 && depth < 20) {
                        depth++;
                        if (lil_guys[cur].is_pioneer) has_pioneer = true;
                        ground = cur;
                        cur = lil_guys[cur].stack_on;
                    }

                    // Mount first — then check if it causes a collapse
                    auto& hopper = lil_guys[hopper_i];
                    hopper.stack_on = mount_on;
                    hopper.stack_hop_remaining = 12;
                    hopper.state = STATE_IDLE;
                    hopper.has_target = false;
                    hopper.has_target_cell = false;
                    hopper.idle_ticks_remaining = g_rng.rand_int(60, 120);
                    hopper.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                    lil_guys[mount_on].interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;

                    // Check collapse: major on pioneer, or too tall
                    bool collapse = false;
                    if (lil_guys[hopper_i].role == ROLE_MAJOR && has_pioneer)
                        collapse = true;
                    int max_depth = lil_guys[ground].is_pioneer ? 5
                                  : (lil_guys[ground].role == ROLE_MAJOR ? 10 : 7);
                    if (depth + 1 > max_depth)
                        collapse = true;

                    if (collapse) {
                        // Disassemble entire stack from ground up
                        for (int j = 0; j < lil_guy_count; j++) {
                            if (lil_guys[j].stack_on < 0) continue;
                            // Check if this ant is in the same stack
                            int walk = lil_guys[j].stack_on;
                            while (walk >= 0) {
                                if (walk == ground) {
                                    lil_guys[j].stack_on = -1;
                                    break;
                                }
                                walk = lil_guys[walk].stack_on;
                            }
                        }
                    }
                }
            };

            // Same-cell pairs
            for (int ai = grid_head[idx]; ai >= 0; ai = grid_next[ai]) {
                for (int bi = grid_next[ai]; bi >= 0; bi = grid_next[bi]) {
                    try_interact(ai, bi);
                }
            }

            // Adjacent cells (4 cardinal)
            const int dx[] = {1, 0, -1, 0};
            const int dy[] = {0, 1, 0, -1};
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx[d], ny = cy + dy[d];
                if (nx < 0 || nx >= Cfg::GRID_WIDTH || ny < 0 || ny >= Cfg::GRID_HEIGHT)
                    continue;
                int nidx = ny * Cfg::GRID_WIDTH + nx;
                if (grid_head[nidx] < 0) continue;
                if (nidx <= idx) continue;

                for (int ai = grid_head[idx]; ai >= 0; ai = grid_next[ai]) {
                    for (int bi = grid_head[nidx]; bi >= 0; bi = grid_next[bi]) {
                        try_interact(ai, bi);
                    }
                }
            }
        }
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
    int last = lil_guy_count - 1;
    lil_guys[idx] = lil_guys[last];
    lil_guy_count--;
    // Patch stack_on references: last moved to idx, idx is gone
    for (int k = 0; k < lil_guy_count; k++) {
        if (lil_guys[k].stack_on == last)      lil_guys[k].stack_on = idx;
        else if (lil_guys[k].stack_on == idx)   lil_guys[k].stack_on = -1;
    }
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
