/* Chamber — ported from sim/chamber.py. */
#include "chamber.h"
#include "rng.h"
#include "time_of_day.h"
#include <Arduino.h>
#include <cstring>
#include <cmath>

void Chamber::init(ColonyState* col, bool with_queen) {
    colony = col;
    conker_count = 0;
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
    // Clear lifecycle notifications from previous tick
    death_count = 0;
    hatch_count = 0;

    // Pheromone decay
    pheromones.decay();

    // Queen beacon — food pheromone only (home direction is known via topology)
    // (no home beacon needed; workers navigate by queen position directly)

    // Queen tick
    if (has_queen) queen_obj.tick(*this, dt);

    // Brood tick — with transition events
    for (int i = brood_count - 1; i >= 0; i--) {
        BroodTransition result = brood[i].tick(dt);
        switch (result) {
        case BROOD_HATCH: {
            bool pioneer = colony->total_workers_born < Cfg::FOUNDING_EGG_COUNT;
            int8_t bx = brood[i].x, by = brood[i].y;
            uint32_t brood_id = brood[i].id;
            add_conker(bx, by, brood[i].role, pioneer);
            // Transfer brood ID to newly-hatched worker
            if (brood_id != 0) {
                conkers[conker_count - 1].id = brood_id;
                if (hatch_count < MAX_LIFECYCLE) {
                    hatch_ids[hatch_count] = brood_id;
                    hatch_tended_by[hatch_count] = brood[i].tended_by;
                    hatch_count++;
                }
            }
            colony->total_workers_born++;
            colony->worker_census++;
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

    // Shuffle workers (Fisher-Yates), fixing stack_on and zoomie_target references
    for (int i = conker_count - 1; i > 0; i--) {
        int j = g_rng.rand_int(0, i);
        if (i != j) {
            Conker tmp = conkers[i]; conkers[i] = conkers[j]; conkers[j] = tmp;
            // Patch any stack_on / zoomie_target references that pointed to i or j
            for (int k = 0; k < conker_count; k++) {
                if (conkers[k].stack_on == i)      conkers[k].stack_on = j;
                else if (conkers[k].stack_on == j) conkers[k].stack_on = i;
                if (conkers[k].zoomie_target == i)      conkers[k].zoomie_target = j;
                else if (conkers[k].zoomie_target == j) conkers[k].zoomie_target = i;
            }
        }
    }

    // Save prev positions
    for (int i = 0; i < conker_count; i++) {
        conkers[i].prev_x = conkers[i].x;
        conkers[i].prev_y = conkers[i].y;
    }

    // Worker ticks + death
    for (int i = conker_count - 1; i >= 0; i--) {
        if (conkers[i].departing) continue;  // frozen, waiting for transfer
        conkers[i].tick(*this, dt);
        if (!conkers[i].alive) {
            // Record death for persistence before removal
            if (conkers[i].id != 0 && death_count < MAX_LIFECYCLE) {
                uint8_t cause = (conkers[i].hunger >= Cfg::HUNGER_STARVE) ? 1 : 0;
                deaths[death_count++] = {conkers[i].id, cause};
            }
            if (conkers[i].food_carried > 0)
                add_food(conkers[i].cell_x(), conkers[i].cell_y(),
                         conkers[i].food_carried);
            Event ev; ev.type = EVT_CONKER_DIED; ev.tick = tick_num;
            emit(ev);
            remove_conker(i);
        }
    }

    // Edge crossing + handoff is handled by Coordinator after chamber.tick()

    // Proximity interactions between lil guys
    _detect_proximity_interactions();

    // Validate stacks: off-screen collapse
    for (int i = 0; i < conker_count; i++) {
        if (conkers[i].stack_on < 0) continue;
        // Compute approximate screen Y with stack offset
        float screen_y = conkers[i].y * Cfg::CELL_SIZE;
        int cur = conkers[i].stack_on;
        while (cur >= 0) {
            float s = (conkers[cur].role == ROLE_MAJOR) ? 4.6f
                    : (conkers[cur].is_pioneer ? 2.3f : 3.2f);
            screen_y -= static_cast<int>(10.0f * s + 0.5f) * 0.75f;
            cur = conkers[cur].stack_on;
        }
        if (screen_y < 28.0f) {  // HUD_STRIP_H
            conkers[i].stack_on = -1;
            conkers[i].sleeping = false;
            conkers[i].anim_type = LG_ANIM_NONE;
        }
    }
}

// ---- proximity interactions ----

void Chamber::_detect_proximity_interactions() {
    if (!event_bus || conker_count < 2) return;

    // Precompute which ants are part of a stack (rider or base)
    static bool in_stack[Cfg::MAX_CONKERS];
    memset(in_stack, 0, sizeof(in_stack));
    for (int i = 0; i < conker_count; i++) {
        if (conkers[i].stack_on >= 0) {
            in_stack[i] = true;                    // rider
            in_stack[conkers[i].stack_on] = true; // base
        }
    }

    // Simple spatial hash: cell -> first worker index, chain via -1 sentinel.
    // For bounded memory, use a fixed grid array.
    static int16_t grid_head[Cfg::GRID_CELLS];
    static int16_t grid_next[Cfg::MAX_CONKERS];
    memset(grid_head, -1, sizeof(grid_head));

    for (int i = 0; i < conker_count; i++) {
        if (!conkers[i].alive) continue;
        int cx = conkers[i].cell_x();
        int cy = conkers[i].cell_y();
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
                auto& a = conkers[ai];
                auto& b = conkers[bi];

                // Trail protection: don't interrupt foraging workers
                bool a_on_job = (a.state == STATE_TO_FOOD)
                             || (a.state == STATE_TO_HOME && a.food_carried > 0);
                bool b_on_job = (b.state == STATE_TO_FOOD)
                             || (b.state == STATE_TO_HOME && b.food_carried > 0);
                if (a_on_job || b_on_job) return;

                // Cooldown gating
                if (a.interaction_cooldown > 0 || b.interaction_cooldown > 0) return;

                // Already animating or sleeping
                if (a.anim_remaining_ticks > 0 || b.anim_remaining_ticks > 0) return;
                if (a.sleeping || b.sleeping) return;

                // Zoomies: daytime only, both idle, not stacked, not already zooming
                if (g_tod.phase == PHASE_DAY
                        && a.state == STATE_IDLE && b.state == STATE_IDLE
                        && !in_stack[ai] && !in_stack[bi]
                        && a.zoomie_ticks <= 0 && b.zoomie_ticks <= 0
                        && g_rng.rand_float() < Cfg::ZOOMIE_CHANCE) {
                    int duration = g_rng.rand_int(Cfg::ZOOMIE_MIN_TICKS,
                                                   Cfg::ZOOMIE_MAX_TICKS);
                    // A = runner (random waypoints), B chases A
                    a.state = STATE_ZOOMIES;
                    a.zoomie_target = -1;  // runner
                    a.zoomie_ticks = duration;
                    a.speed = Cfg::ROLE_PARAMS[a.role].speed * Cfg::ZOOMIE_SPEED_MULT;
                    a.has_target = false;
                    a.has_target_cell = false;
                    a.idle_ticks_remaining = 0;

                    b.state = STATE_ZOOMIES;
                    b.zoomie_target = ai;  // chases A
                    b.zoomie_ticks = duration;
                    b.speed = Cfg::ROLE_PARAMS[b.role].speed * Cfg::ZOOMIE_SPEED_MULT;
                    b.has_target = false;
                    b.has_target_cell = false;
                    b.idle_ticks_remaining = 0;

                    // Try to recruit a third nearby idle lil guy
                    if (g_rng.rand_float() < Cfg::ZOOMIE_THIRD_CHANCE) {
                        int acx = a.cell_x(), acy = a.cell_y();
                        for (int ci = 0; ci < conker_count; ci++) {
                            if (ci == ai || ci == bi) continue;
                            auto& c = conkers[ci];
                            if (!c.alive || c.state != STATE_IDLE || c.sleeping) continue;
                            if (in_stack[ci] || c.anim_remaining_ticks > 0) continue;
                            if (c.interaction_cooldown > 0 || c.zoomie_ticks > 0) continue;
                            int d = abs(c.cell_x() - acx) + abs(c.cell_y() - acy);
                            if (d <= 3) {
                                // Chain: A runs, B chases A, C chases B
                                c.state = STATE_ZOOMIES;
                                c.zoomie_target = bi;  // chases B
                                c.zoomie_ticks = duration;
                                c.speed = Cfg::ROLE_PARAMS[c.role].speed * Cfg::ZOOMIE_SPEED_MULT;
                                c.has_target = false;
                                c.has_target_cell = false;
                                c.idle_ticks_remaining = 0;
                                break;
                            }
                        }
                    }
                    return;
                }

                // Food sharing and grooming only between non-stacked ants
                if (!in_stack[ai] && !in_stack[bi] &&
                    (a.food_carried > 0) != (b.food_carried > 0)) {
                    // social_frequency biases food share chance
                    float social = (a.personality[PERS_SOCIAL_FREQUENCY]
                                  + b.personality[PERS_SOCIAL_FREQUENCY]) * 0.5f;
                    float share_chance = Cfg::PROXIMITY_FOOD_SHARE_CHANCE
                                       * (0.5f + social);  // 0.5x to 1.5x
                    if (g_rng.rand_float() < share_chance) {
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
                // social_frequency biases greeting chance
                float greet_social = (a.personality[PERS_SOCIAL_FREQUENCY]
                                    + b.personality[PERS_SOCIAL_FREQUENCY]) * 0.5f;
                float greet_chance = Cfg::PROXIMITY_GREETING_CHANCE
                                   * (0.5f + greet_social);
                if (g_rng.rand_float() < greet_chance) {
                    if (!in_stack[ai] && !in_stack[bi] && g_rng.rand_float() < 0.5f) {
                        // Mutual grooming: both lean toward each other (non-stacked only)
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
                            for (int j = 0; j < conker_count; j++)
                                if (conkers[j].alive && conkers[j].stack_on == idx)
                                    { found = j; break; }
                            if (found < 0) return idx;
                            idx = found;
                        }
                    };

                    // Collapse cooldown: don't stack if either ant recently collapsed
                    uint32_t now_ms = millis();
                    if (a.stack_cooldown_ms > now_ms || b.stack_cooldown_ms > now_ms)
                        return;

                    // Pick who becomes the base, who hops on
                    // An ant already riding can't hop (would break chain)
                    int base_i, hopper_i;
                    bool a_riding = (a.stack_on >= 0), b_riding = (b.stack_on >= 0);
                    if (a_riding && b_riding) return;  // both already riding
                    if (a_riding) {
                        base_i = ai; hopper_i = bi;    // rider becomes base
                    } else if (b_riding) {
                        base_i = bi; hopper_i = ai;
                    } else if (g_rng.rand_float() < 0.5f) {
                        base_i = ai; hopper_i = bi;
                    } else {
                        base_i = bi; hopper_i = ai;
                    }
                    int mount_on = top_of(base_i);

                    // Cycle check: ensure hopper isn't already below mount_on
                    bool cycle = false;
                    for (int check = mount_on; check >= 0; check = conkers[check].stack_on)
                        if (check == hopper_i) { cycle = true; break; }
                    if (cycle) return;

                    auto weight_of = [&](int idx) -> int {
                        if (conkers[idx].is_pioneer) return Cfg::STACK_WEIGHT_PIONEER;
                        return (conkers[idx].role == ROLE_MAJOR) ? Cfg::STACK_WEIGHT_MAJOR
                                                                   : Cfg::STACK_WEIGHT_MINOR;
                    };

                    // Mount first
                    auto& hopper = conkers[hopper_i];
                    hopper.stack_on = mount_on;
                    hopper.stack_hop_remaining = 12;
                    hopper.state = STATE_IDLE;
                    hopper.has_target = false;
                    hopper.has_target_cell = false;
                    hopper.idle_ticks_remaining = g_rng.rand_int(60, 120);
                    hopper.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                    conkers[mount_on].interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;

                    // Walk the full combined stack from true top to true ground
                    int true_top = top_of(hopper_i);
                    int stack_weight = 0;
                    bool has_pioneer = false;
                    int ground = true_top;
                    int cur = true_top;
                    while (cur >= 0) {
                        stack_weight += weight_of(cur);
                        if (conkers[cur].is_pioneer) has_pioneer = true;
                        ground = cur;
                        cur = conkers[cur].stack_on;
                    }
                    stack_weight -= weight_of(ground);  // ground supports, isn't supported

                    // Check collapse: major on pioneer, or too heavy
                    bool collapse = false;
                    if (conkers[hopper_i].role == ROLE_MAJOR && has_pioneer)
                        collapse = true;
                    int capacity = conkers[ground].is_pioneer ? Cfg::STACK_CAPACITY_PIONEER
                                 : (conkers[ground].role == ROLE_MAJOR ? Cfg::STACK_CAPACITY_MAJOR
                                                                         : Cfg::STACK_CAPACITY_MINOR);
                    if (stack_weight > capacity)
                        collapse = true;

                    if (collapse) {
                        // Start topple animation on entire stack
                        for (int j = 0; j < conker_count; j++) {
                            // Check if this ant is in the stack (including ground)
                            bool in_this_stack = (j == ground);
                            if (!in_this_stack && conkers[j].stack_on >= 0) {
                                int walk = conkers[j].stack_on;
                                while (walk >= 0) {
                                    if (walk == ground) { in_this_stack = true; break; }
                                    walk = conkers[walk].stack_on;
                                }
                            }
                            if (in_this_stack) {
                                // Compute depth: count ants below this one
                                int depth = 0;
                                int walk = conkers[j].stack_on;
                                while (walk >= 0) { depth++; walk = conkers[walk].stack_on; }
                                conkers[j].topple_depth = depth;
                                conkers[j].anim_type = LG_ANIM_TOPPLE;
                                conkers[j].anim_remaining_ticks =
                                    Cfg::STACK_TOPPLE_TICKS + Cfg::STACK_FALL_TICKS;
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

void Chamber::add_conker(int8_t px, int8_t py, Role c, bool pioneer) {
    if (conker_count >= Cfg::MAX_CONKERS) return;
    conkers[conker_count].init(px, py, c, pioneer);
    conker_count++;
}

void Chamber::remove_conker(int idx) {
    if (idx < 0 || idx >= conker_count) return;
    int last = conker_count - 1;
    conkers[idx] = conkers[last];
    conker_count--;
    // Patch stack_on and zoomie_target references: last moved to idx, idx is gone
    for (int k = 0; k < conker_count; k++) {
        if (conkers[k].stack_on == last)      conkers[k].stack_on = idx;
        else if (conkers[k].stack_on == idx) {
            conkers[k].stack_on = -1;
            conkers[k].sleeping = false;
            conkers[k].anim_type = LG_ANIM_NONE;
        }
        if (conkers[k].zoomie_target == last)      conkers[k].zoomie_target = idx;
        else if (conkers[k].zoomie_target == idx)   conkers[k].zoomie_target = -1;
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
        // N/S faces span horizontally, W/E faces span vertically
        bool on_edge = (Cfg::FACE_DY[f] != 0)
            ? (y == Cfg::ENTRY_Y[f] && abs(x - Cfg::ENTRY_X[f]) <= Cfg::ENTRY_HALF_W)
            : (x == Cfg::ENTRY_X[f] && abs(y - Cfg::ENTRY_Y[f]) <= Cfg::ENTRY_HALF_W);
        if (on_edge) return f;
    }
    return -1;
}
