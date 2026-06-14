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
    husk_count = 0;
    for (int i = 0; i < Cfg::MAX_FIREFLIES; i++) fireflies[i] = Firefly{};
    for (int i = 0; i < Cfg::MAX_CRITTERS; i++) critters[i] = Critter{};
    for (int i = 0; i < Cfg::MAX_SCENT_MARKS; i++) scent_marks[i] = {0, 0, 0};
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
            bool founder = colony->total_workers_born < Cfg::FOUNDER_COHORT_SIZE;
            int8_t bx = brood[i].x, by = brood[i].y;
            uint32_t brood_id = brood[i].id;
            add_conker(bx, by, ROLE_CONKER, false);
            // Transfer brood ID and set founder tag
            if (brood_id != 0) {
                conkers[conker_count - 1].id = brood_id;
                if (hatch_count < MAX_LIFECYCLE) {
                    hatch_ids[hatch_count] = brood_id;
                    hatch_tended_by[hatch_count] = brood[i].tended_by;
                    hatch_count++;
                }
            }
            conkers[conker_count - 1].is_founder = founder;
            colony->total_workers_born++;
            colony->worker_census++;
            if (!queen_obj.founding_done) queen_obj.founding_done = true;
            Event ev; ev.type = EVT_YOUNG_HATCHED; ev.tick = tick_num;
            ev.young_hatched = {STAGE_SEED, 0xFF, bx, by};
            emit(ev);
            remove_brood(i);
            break;
        }
        case BROOD_EGG_TO_SEED: {
            Event ev; ev.type = EVT_YOUNG_HATCHED; ev.tick = tick_num;
            ev.young_hatched = {STAGE_EGG, STAGE_SEED, brood[i].x, brood[i].y};
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
            if (brood[i].stage == STAGE_SEED && brood[i].alive()
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

    // Night ambience
    _tick_fireflies();
    _tick_critters();
    _detect_critter_discovery();

    // Finger scent shimmer fade
    for (int i = 0; i < Cfg::MAX_SCENT_MARKS; i++)
        if (scent_marks[i].ttl > 0) scent_marks[i].ttl--;

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
            // Leave a husk at death location
            add_husk(static_cast<int8_t>(conkers[i].cell_x()),
                     static_cast<int8_t>(conkers[i].cell_y()),
                     conkers[i].id, conkers[i].scale_factor, g_tod.unix_time);
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

    // Validate stacks: stale/cyclic references collapse, then off-screen collapse.
    // Every chain walk is hop-bounded — a stack_on cycle must degrade to a
    // dismount, never an infinite loop (this hang took out a whole module once).
    for (int i = 0; i < conker_count; i++) {
        if (conkers[i].stack_on < 0) continue;
        if (conkers[i].stack_on >= conker_count) {  // dangling reference
            conkers[i].stack_on = -1;
            conkers[i].sleeping = false;
            conkers[i].anim_type = LG_ANIM_NONE;
            continue;
        }
        // Compute approximate screen Y with stack offset
        float screen_y = conkers[i].y * Cfg::CELL_SIZE;
        int cur = conkers[i].stack_on;
        int hops = 0;
        while (cur >= 0 && cur < conker_count && ++hops <= Cfg::MAX_CONKERS) {
            float s = conkers[cur].scale_factor;
            screen_y -= static_cast<int>(10.0f * s + 0.5f) * 0.75f;
            cur = conkers[cur].stack_on;
        }
        if (screen_y < 28.0f || hops > Cfg::MAX_CONKERS) {  // HUD_STRIP_H or cycle
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

                // Trail courtesy: an outbound forager pauses and steps aside
                // for a loaded carrier. Only the empty-handed one reacts —
                // the carrier never breaks stride.
                bool a_carrying = (a.state == STATE_TO_HOME && a.food_carried > 0);
                bool b_carrying = (b.state == STATE_TO_HOME && b.food_carried > 0);
                bool a_outbound = (a.state == STATE_TO_FOOD && a.food_carried <= 0);
                bool b_outbound = (b.state == STATE_TO_FOOD && b.food_carried <= 0);
                if ((a_carrying && b_outbound) || (b_carrying && a_outbound)) {
                    auto& walker  = a_carrying ? b : a;
                    auto& carrier = a_carrying ? a : b;
                    if (walker.interaction_cooldown == 0
                            && walker.anim_remaining_ticks == 0
                            && walker.flair_ticks == 0
                            && colony->food_pressure() <= Cfg::FLAIR_MAX_PRESSURE
                            && g_rng.rand_float() < Cfg::COURTESY_YIELD_CHANCE) {
                        float ddx = carrier.x - walker.x, ddy = carrier.y - walker.y;
                        float d = sqrtf(ddx * ddx + ddy * ddy);
                        if (d > 0.01f) {
                            walker.facing_dx = ddx / d; walker.facing_dy = ddy / d;
                            walker.last_dx = walker.facing_dx;
                            walker.last_dy = walker.facing_dy;
                        }
                        walker.has_target_cell = false;
                        walker.flair_kind = 3;  // courtesy hold (stand + watch)
                        walker.flair_ticks = Cfg::COURTESY_YIELD_TICKS;
                        walker.interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
                    }
                    return;
                }

                if (a_on_job || b_on_job) return;

                // Cooldown gating
                if (a.interaction_cooldown > 0 || b.interaction_cooldown > 0) return;

                // Already animating or sleeping
                if (a.anim_remaining_ticks > 0 || b.anim_remaining_ticks > 0) return;
                if (a.sleeping || b.sleeping) return;

                // Grief is private — no games or greetings around mourners
                if (a.state == STATE_MOURNING || b.state == STATE_MOURNING) return;

                // A passing game sweeps bystanders in: an idler may join a
                // runner's sprint or a parade as it goes by
                {
                    bool a_playing = (a.state == STATE_ZOOMIES && a.zoomie_target >= -1);
                    bool b_playing = (b.state == STATE_ZOOMIES && b.zoomie_target >= -1);
                    if (a_playing != b_playing && g_tod.phase == PHASE_DAY) {
                        auto& runner = a_playing ? a : b;
                        auto& joiner = a_playing ? b : a;
                        int runner_idx = a_playing ? ai : bi;
                        int joiner_idx = a_playing ? bi : ai;
                        if (joiner.state == STATE_IDLE && !joiner.sleeping
                                && !in_stack[joiner_idx]
                                && joiner.zoomie_ticks <= 0
                                && runner.zoomie_ticks > 8
                                && g_rng.rand_float() < Cfg::ZOOMIE_JOIN_CHANCE) {
                            joiner.state = STATE_ZOOMIES;
                            joiner.zoomie_target = runner_idx;
                            joiner.zoomie_style = runner.zoomie_style;
                            joiner.zoomie_ticks = runner.zoomie_ticks;
                            joiner.speed = Cfg::ROLE_PARAMS[joiner.role].speed
                                * (runner.zoomie_style == 1 ? Cfg::PARADE_SPEED_MULT
                                                            : Cfg::ZOOMIE_SPEED_MULT);
                            joiner.has_target = false;
                            joiner.has_target_cell = false;
                            joiner.idle_ticks_remaining = 0;
                            Serial.printf("[zoomies] %s joins %s's game\r\n",
                                          joiner.name, runner.name);
                            return;
                        }
                    }
                }

                // Zoomies & parades: daytime only, both idle, not stacked,
                // not already zooming. A deep pantry makes play more likely
                // (the hands freed from foraging) and sometimes turns the
                // chase into a follow-the-leader parade.
                float surplus = colony->play_surplus();
                if (g_tod.phase == PHASE_DAY
                        && a.state == STATE_IDLE && b.state == STATE_IDLE
                        && !in_stack[ai] && !in_stack[bi]
                        && a.zoomie_ticks <= 0 && b.zoomie_ticks <= 0
                        && g_rng.rand_float() < Cfg::ZOOMIE_CHANCE
                                * (1.0f + Cfg::ZOOMIE_SURPLUS_BOOST * surplus)) {
                    bool parade = surplus >= Cfg::PARADE_MIN_SURPLUS
                               && g_rng.rand_float() < Cfg::PARADE_CHANCE;
                    int duration = parade
                        ? g_rng.rand_int(Cfg::PARADE_MIN_TICKS, Cfg::PARADE_MAX_TICKS)
                        : g_rng.rand_int(Cfg::ZOOMIE_MIN_TICKS,
                                         Cfg::ZOOMIE_MAX_TICKS
                                         + static_cast<int>(surplus * Cfg::ZOOMIE_SURPLUS_TICKS));
                    uint8_t style = parade ? 1 : 0;
                    float   mult  = parade ? Cfg::PARADE_SPEED_MULT : Cfg::ZOOMIE_SPEED_MULT;

                    // A = leader/runner (random waypoints), B follows A
                    a.state = STATE_ZOOMIES;
                    a.zoomie_target = -1;  // runner
                    a.zoomie_style = style;
                    a.zoomie_ticks = duration;
                    a.speed = Cfg::ROLE_PARAMS[a.role].speed * mult;
                    a.has_target = false;
                    a.has_target_cell = false;
                    a.idle_ticks_remaining = 0;

                    b.state = STATE_ZOOMIES;
                    b.zoomie_target = ai;  // follows A
                    b.zoomie_style = style;
                    b.zoomie_ticks = duration;
                    b.speed = Cfg::ROLE_PARAMS[b.role].speed * mult;
                    b.has_target = false;
                    b.has_target_cell = false;
                    b.idle_ticks_remaining = 0;

                    // Recruit more: a parade chains followers behind the
                    // tail; a chase sometimes picks up a third
                    int participants = 2;
                    int tail = bi;
                    int max_extra = parade ? Cfg::PARADE_MAX_FOLLOWERS - 1 : 1;
                    if (parade || g_rng.rand_float() < Cfg::ZOOMIE_THIRD_CHANCE) {
                        int acx = a.cell_x(), acy = a.cell_y();
                        int radius = parade ? Cfg::PARADE_RECRUIT_RADIUS : 3;
                        for (int ci = 0; ci < conker_count
                                && participants - 2 < max_extra; ci++) {
                            if (ci == ai || ci == bi) continue;
                            auto& c = conkers[ci];
                            if (!c.alive || c.state != STATE_IDLE || c.sleeping) continue;
                            if (in_stack[ci] || c.anim_remaining_ticks > 0) continue;
                            if (c.interaction_cooldown > 0 || c.zoomie_ticks > 0) continue;
                            int d = abs(c.cell_x() - acx) + abs(c.cell_y() - acy);
                            if (d <= radius) {
                                c.state = STATE_ZOOMIES;
                                c.zoomie_target = tail;  // each follows the one before
                                c.zoomie_style = style;
                                c.zoomie_ticks = duration;
                                c.speed = Cfg::ROLE_PARAMS[c.role].speed * mult;
                                c.has_target = false;
                                c.has_target_cell = false;
                                c.idle_ticks_remaining = 0;
                                tail = ci;
                                participants++;
                            }
                        }
                    }

                    if (parade) {
                        Serial.printf("[parade] %s leads %d others on a parade (%d ticks)\r\n",
                                      a.name, participants - 1, duration);
                        if (event_bus) {
                            Event ev = {};
                            ev.type = EVT_PARADE_STARTED;
                            ev.tick = tick_num;
                            ev.parade.leader_idx = static_cast<uint8_t>(ai);
                            ev.parade.participants = static_cast<uint8_t>(participants);
                            event_bus->emit(ev);
                        }
                    } else {
                        Serial.printf("[zoomies] %s chases %s (%d ticks)\r\n",
                                      b.name, a.name, duration);
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
                    // Elders always get the grooming branch — respected, not climbed on
                    bool elder_present = a.is_elder() || b.is_elder();
                    if (!in_stack[ai] && !in_stack[bi]
                            && (elder_present || g_rng.rand_float() < 0.5f)) {
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

                    // Find the topmost ant in a tower (hop-bounded: a corrupt
                    // cycle returns the current idx instead of spinning forever)
                    auto top_of = [&](int idx) -> int {
                        for (int hops = 0; hops < conker_count; hops++) {
                            int found = -1;
                            for (int j = 0; j < conker_count; j++)
                                if (conkers[j].alive && conkers[j].stack_on == idx)
                                    { found = j; break; }
                            if (found < 0) return idx;
                            idx = found;
                        }
                        return idx;
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
                    int check_hops = 0;
                    for (int check = mount_on; check >= 0 && check < conker_count;
                         check = conkers[check].stack_on) {
                        if (check == hopper_i || ++check_hops > conker_count)
                            { cycle = true; break; }
                    }
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
                    int walk_hops = 0;
                    while (cur >= 0 && cur < conker_count && ++walk_hops <= conker_count) {
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
                                int wh = 0;
                                while (walk >= 0 && walk < conker_count && ++wh <= conker_count) {
                                    if (walk == ground) { in_this_stack = true; break; }
                                    walk = conkers[walk].stack_on;
                                }
                            }
                            if (in_this_stack) {
                                // Compute depth: count ants below this one
                                int depth = 0;
                                int walk = conkers[j].stack_on;
                                while (walk >= 0 && walk < conker_count && depth <= conker_count)
                                    { depth++; walk = conkers[walk].stack_on; }
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
    // Patch stack_on and zoomie_target references: last moved to idx, idx is gone.
    // The removed-index check must come FIRST: when idx == last, a rider of the
    // removed conker would otherwise be "patched" to the same dead index and
    // dangle into whatever fills that slot next (stack_on cycles → hang).
    for (int k = 0; k < conker_count; k++) {
        if (conkers[k].stack_on == idx) {
            conkers[k].stack_on = -1;
            conkers[k].sleeping = false;
            conkers[k].anim_type = LG_ANIM_NONE;
        } else if (conkers[k].stack_on == last) {
            conkers[k].stack_on = idx;
        }
        if (conkers[k].zoomie_target == idx)       conkers[k].zoomie_target = -1;
        else if (conkers[k].zoomie_target == last) conkers[k].zoomie_target = idx;
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

void Chamber::count_brood(uint16_t& eggs, uint16_t& seeds) const {
    eggs = seeds = 0;
    for (int i = 0; i < brood_count; i++) {
        if (!brood[i].alive()) continue;
        switch (brood[i].stage) {
            case STAGE_EGG:  eggs++;  break;
            case STAGE_SEED: seeds++; break;
            default: break;
        }
    }
}

void Chamber::add_brood_with_duration(int8_t px, int8_t py, uint32_t duration_ms) {
    if (brood_count >= Cfg::MAX_BROOD) return;
    brood[brood_count].init_with_duration(px, py, duration_ms);
    brood_count++;
}

// ---- Fireflies ----

void Chamber::_tick_fireflies() {
    bool night = g_tod.night_factor >= Cfg::FIREFLY_NIGHT_FACTOR_MIN;

    for (int i = 0; i < Cfg::MAX_FIREFLIES; i++) {
        Firefly& f = fireflies[i];

        if (!f.active) {
            if (night && g_rng.rand_float() < Cfg::FIREFLY_SPAWN_CHANCE) {
                f.active = true;
                f.x = f.prev_x = static_cast<float>(g_rng.rand_int(2, Cfg::GRID_WIDTH - 3));
                f.y = f.prev_y = static_cast<float>(g_rng.rand_int(2, Cfg::GRID_HEIGHT - 3));
                f.vx = f.vy = 0.0f;
                f.glow_phase = static_cast<uint8_t>(g_rng.rand_int(0, 255));
            }
            continue;
        }

        // Daybreak — fireflies retire
        if (!night) { f.active = false; continue; }

        f.prev_x = f.x;
        f.prev_y = f.y;

        // Flee any conker that's too close; otherwise lazy drift
        float fx = 0.0f, fy = 0.0f;
        bool fleeing = false;
        for (int c = 0; c < conker_count; c++) {
            Conker& w = conkers[c];
            if (!w.alive) continue;
            float dx = f.x - w.x, dy = f.y - w.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < 4.0f && d2 > 0.0001f) {  // within 2 cells
                float d = sqrtf(d2);
                fx += dx / d; fy += dy / d;
                fleeing = true;
            }
        }
        if (fleeing) {
            float m = sqrtf(fx * fx + fy * fy);
            if (m > 0.001f) {
                f.vx = fx / m * Cfg::FIREFLY_FLEE_SPEED;
                f.vy = fy / m * Cfg::FIREFLY_FLEE_SPEED;
            }
        } else {
            f.vx += (g_rng.rand_float() - 0.5f) * 0.02f;
            f.vy += (g_rng.rand_float() - 0.5f) * 0.02f;
            float sp = sqrtf(f.vx * f.vx + f.vy * f.vy);
            if (sp > Cfg::FIREFLY_SPEED) {
                f.vx *= Cfg::FIREFLY_SPEED / sp;
                f.vy *= Cfg::FIREFLY_SPEED / sp;
            }
        }

        f.x += f.vx;
        f.y += f.vy;
        if (f.x < 1.0f) { f.x = 1.0f; f.vx = fabsf(f.vx); }
        if (f.x > Cfg::GRID_WIDTH - 2.0f)  { f.x = Cfg::GRID_WIDTH - 2.0f;  f.vx = -fabsf(f.vx); }
        if (f.y < 1.0f) { f.y = 1.0f; f.vy = fabsf(f.vy); }
        if (f.y > Cfg::GRID_HEIGHT - 2.0f) { f.y = Cfg::GRID_HEIGHT - 2.0f; f.vy = -fabsf(f.vy); }

        f.glow_phase += 3;  // wraps — ~10s blink cycle at 8tps
    }
}

void Chamber::_tick_critters() {
    bool day = (g_tod.phase == PHASE_DAY || g_tod.phase == PHASE_DUSK
             || g_tod.phase == PHASE_DAWN);

    int active = 0;
    for (int i = 0; i < Cfg::MAX_CRITTERS; i++)
        if (critters[i].active) active++;

    for (int i = 0; i < Cfg::MAX_CRITTERS; i++) {
        Critter& cr = critters[i];

        if (!cr.active) {
            // One visitor wanders in at a time, only in daylight.
            if (day && active == 0 && conker_count > 0
                    && g_rng.rand_float() < Cfg::CRITTER_SPAWN_CHANCE) {
                cr.active = true;
                cr.flee = 0;
                cr.ttl = static_cast<uint16_t>(
                    g_rng.rand_int(Cfg::CRITTER_TTL_MIN, Cfg::CRITTER_TTL_MAX));
                float r = g_rng.rand_float();
                cr.kind = (r < 0.50f) ? CRITTER_BUTTERFLY
                        : (r < 0.85f) ? CRITTER_BEETLE
                                      : CRITTER_WORM;
                // Enter from a random edge
                if (g_rng.rand_int(0, 1) == 0) {
                    cr.x = (g_rng.rand_int(0, 1) == 0) ? 1.0f : Cfg::GRID_WIDTH - 2.0f;
                    cr.y = static_cast<float>(g_rng.rand_int(2, Cfg::GRID_HEIGHT - 3));
                } else {
                    cr.x = static_cast<float>(g_rng.rand_int(2, Cfg::GRID_WIDTH - 3));
                    cr.y = (g_rng.rand_int(0, 1) == 0) ? 1.0f : Cfg::GRID_HEIGHT - 2.0f;
                }
                cr.prev_x = cr.x; cr.prev_y = cr.y;
                cr.vx = cr.vy = 0.0f;
                cr.anim_phase = 0;
                active++;
            }
            continue;
        }

        cr.prev_x = cr.x;
        cr.prev_y = cr.y;
        cr.anim_phase++;

        if (cr.flee > 0) {
            // Scurry off after being found, then vanish at the edge / timeout
            cr.x += cr.vx; cr.y += cr.vy;
            cr.flee--;
            if (cr.flee == 0) { cr.active = false; }
            continue;
        }

        if (cr.ttl > 0) cr.ttl--;
        if (cr.ttl == 0) { cr.active = false; continue; }  // wandered off unseen

        float max_speed = (cr.kind == CRITTER_BUTTERFLY) ? Cfg::CRITTER_SPEED_BUTTERFLY
                        : (cr.kind == CRITTER_WORM)       ? Cfg::CRITTER_SPEED_WORM
                                                          : Cfg::CRITTER_SPEED_BEETLE;
        // Wander, with a gentle pull toward the nearest conker so encounters
        // actually happen (you watch the bug amble into the group).
        float jitter = (cr.kind == CRITTER_BUTTERFLY) ? 0.05f : 0.02f;
        cr.vx += (g_rng.rand_float() - 0.5f) * jitter;
        cr.vy += (g_rng.rand_float() - 0.5f) * jitter;
        int nx = -1; float best_d2 = 1e9f;
        for (int c = 0; c < conker_count; c++) {
            if (!conkers[c].alive || conkers[c].departing) continue;
            float dx = conkers[c].x - cr.x, dy = conkers[c].y - cr.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; nx = c; }
        }
        if (nx >= 0 && best_d2 > 0.01f) {
            float d = sqrtf(best_d2);
            cr.vx += (conkers[nx].x - cr.x) / d * Cfg::CRITTER_SEEK_CONKER;
            cr.vy += (conkers[nx].y - cr.y) / d * Cfg::CRITTER_SEEK_CONKER;
        }
        float sp = sqrtf(cr.vx * cr.vx + cr.vy * cr.vy);
        if (sp > max_speed) { cr.vx *= max_speed / sp; cr.vy *= max_speed / sp; }

        cr.x += cr.vx; cr.y += cr.vy;
        if (cr.x < 1.0f) { cr.x = 1.0f; cr.vx = fabsf(cr.vx); }
        if (cr.x > Cfg::GRID_WIDTH - 2.0f)  { cr.x = Cfg::GRID_WIDTH - 2.0f;  cr.vx = -fabsf(cr.vx); }
        if (cr.y < 1.0f) { cr.y = 1.0f; cr.vy = fabsf(cr.vy); }
        if (cr.y > Cfg::GRID_HEIGHT - 2.0f) { cr.y = Cfg::GRID_HEIGHT - 2.0f; cr.vy = -fabsf(cr.vy); }
    }
}

void Chamber::_detect_critter_discovery() {
    for (int i = 0; i < Cfg::MAX_CRITTERS; i++) {
        Critter& cr = critters[i];
        if (!cr.active || cr.flee > 0) continue;

        int finder = -1; float best_d2 = Cfg::CRITTER_FIND_RADIUS * Cfg::CRITTER_FIND_RADIUS;
        for (int c = 0; c < conker_count; c++) {
            Conker& w = conkers[c];
            if (!w.alive || w.departing || w.sleeping) continue;
            float dx = w.x - cr.x, dy = w.y - cr.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; finder = c; }
        }
        if (finder < 0) continue;

        // Discovery! Novelty relieves boredom, the finder startles, and a
        // couple of bystanders come to look — a little commotion.
        Conker& f = conkers[finder];
        f.needs[NEED_BOREDOM] -= Cfg::DISCOVERY_BOREDOM_RELIEF;
        if (f.needs[NEED_BOREDOM] < 0.0f) f.needs[NEED_BOREDOM] = 0.0f;
        if (f.anim_remaining_ticks == 0 && f.state == STATE_IDLE && f.stack_on < 0) {
            f.anim_type = LG_ANIM_NOTICE;
            f.anim_remaining_ticks = 12;
        }
        int gawkers = 0;
        for (int c = 0; c < conker_count && gawkers < 2; c++) {
            if (c == finder) continue;
            Conker& w = conkers[c];
            if (!w.alive || w.sleeping || w.stack_on >= 0) continue;
            if (w.anim_remaining_ticks != 0 || w.state != STATE_IDLE) continue;
            float dx = w.x - cr.x, dy = w.y - cr.y;
            if (dx * dx + dy * dy > 16.0f) continue;  // within ~4 cells
            w.anim_type = LG_ANIM_NOTICE;
            w.anim_remaining_ticks = 12;
            gawkers++;
        }

        // The critter bolts away from the finder, then despawns.
        float dx = cr.x - f.x, dy = cr.y - f.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 0.001f) { dx = 1.0f; dy = 0.0f; d = 1.0f; }
        cr.vx = dx / d * Cfg::CRITTER_FLEE_SPEED;
        cr.vy = dy / d * Cfg::CRITTER_FLEE_SPEED;
        cr.flee = Cfg::CRITTER_FLEE_TICKS;

        Event ev;
        ev.type = EVT_DISCOVERY;
        ev.tick = tick_num;
        ev.discovery = { static_cast<uint8_t>(finder), cr.kind };
        emit(ev);
    }
}

void Chamber::add_scent_mark(int cx, int cy) {
    // Reuse an expired slot, else the dimmest one
    int slot = 0;
    uint8_t min_ttl = 255;
    for (int i = 0; i < Cfg::MAX_SCENT_MARKS; i++) {
        if (scent_marks[i].ttl == 0) { slot = i; break; }
        if (scent_marks[i].ttl < min_ttl) { min_ttl = scent_marks[i].ttl; slot = i; }
    }
    scent_marks[slot] = {static_cast<int8_t>(cx), static_cast<int8_t>(cy),
                         static_cast<uint8_t>(Cfg::SCENT_MARK_TTL)};
}

int Chamber::nearest_firefly(int cx, int cy, int radius) const {
    int best = -1, best_d = radius + 1;
    for (int i = 0; i < Cfg::MAX_FIREFLIES; i++) {
        if (!fireflies[i].active) continue;
        int d = abs(static_cast<int>(fireflies[i].x) - cx)
              + abs(static_cast<int>(fireflies[i].y) - cy);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

void Chamber::add_husk(int8_t px, int8_t py, uint32_t conker_id, float scale, uint32_t died_unix) {
    if (husk_count >= Cfg::MAX_HUSKS) {
        // Evict oldest
        husks[0] = husks[husk_count - 1];
        husk_count--;
    }
    husks[husk_count++] = {px, py, conker_id, died_unix, scale};
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
