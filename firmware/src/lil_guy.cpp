/* Worker lil_guy -- full behavior port with smooth sub-cell movement. */
#include "lil_guy.h"
#include "chamber.h"
#include "rng.h"
#include "time_of_day.h"
#include <Arduino.h>
#include <cmath>

void LilGuy::init(int8_t px, int8_t py, Role c, bool pioneer) {
    x = float(px) + 0.5f;
    y = float(py) + 0.5f;
    prev_x = x;
    prev_y = y;
    state = STATE_IDLE;
    has_target = false;
    has_target_cell = false;
    target_cell_x = 0;
    target_cell_y = 0;
    born_at_ms = millis();
    alive = true;

    facing_dx = float(g_rng.rand_sign());
    facing_dy = 0.0f;
    last_dx = facing_dx;
    last_dy = 0.0f;
    food_carried = 0.0f;
    steps_walked = 0;
    ticks_away = 0;
    stall_ticks = 0;
    idle_cooldown = 0;
    chamber_steps = 0;
    hunger = 0.0f;

    last_cell_x = px;
    last_cell_y = py;

    idle_ticks_remaining = 0;
    idle_repoll_tick = 0;
    idle_microstate = 0;
    idle_micro_ticks = 0;

    role = c;
    is_pioneer = pioneer;
    const auto& p = Cfg::ROLE_PARAMS[c];
    move_ticks   = p.move_ticks;
    sense_radius = p.sense_radius;
    carry_amount = p.carry_amount;
    speed        = p.speed;

    float lifespan_days;
    if (pioneer)
        lifespan_days = g_rng.rand_gaussian(Cfg::PIONEER_LIFESPAN_MEAN, Cfg::PIONEER_LIFESPAN_SD);
    else
        lifespan_days = g_rng.rand_gaussian(Cfg::WORKER_LIFESPAN_MEAN, Cfg::WORKER_LIFESPAN_SD);
    if (lifespan_days < 1.0f) lifespan_days = 1.0f;  // clamp minimum
    lifespan_ms = static_cast<uint32_t>(lifespan_days * Cfg::SECS_PER_DAY * 1000.0f);
}

// ================================================================
//  Per-tick entry point -- two-phase architecture
// ================================================================

void LilGuy::tick(Chamber& ch, float dt) {
    if (!alive) return;

    if (millis() - born_at_ms >= lifespan_ms) {
        alive = false;
        Event ev; ev.type = EVT_LIL_GUY_DIED; ev.tick = ch.tick_num;
        ev.position = {static_cast<int8_t>(cell_x()), static_cast<int8_t>(cell_y())};
        ch.emit(ev);
        return;
    }

    // Metabolism — centralized in sim.cpp via daily_burn()
    // Here we only manage hunger
    if (ch.colony->food_store > 0 || food_carried > 0) {
        if (hunger > 0.0f) {
            hunger = fmaxf(0.0f, hunger - dt / (Cfg::WORKER_SURVIVAL_DAYS * Cfg::SECS_PER_DAY));
        }
    } else {
        hunger += dt / (Cfg::WORKER_SURVIVAL_DAYS * Cfg::SECS_PER_DAY);
        if (hunger >= 1.0f) {
            alive = false;
            Event ev; ev.type = EVT_LIL_GUY_DIED; ev.tick = ch.tick_num;
            ev.position = {static_cast<int8_t>(cell_x()), static_cast<int8_t>(cell_y())};
            ch.emit(ev);
            return;
        }
    }

    // Track time away from queen chamber
    if (ch.has_queen) ticks_away = 0;
    else ticks_away++;

    // Return-home timer
    if (ticks_away >= Cfg::RETURN_HOME_TICKS && state != STATE_TO_HOME) {
        state = STATE_TO_HOME;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        facing_dx = -facing_dx;
        facing_dy = -facing_dy;
    }

    // Movement phase -- every tick
    _advance_toward_target(ch);

    // Decision phase -- only when arrived (no pending target)
    if (state == STATE_IDLE) {
        if (idle_ticks_remaining > 0) {
            _tick_idle(ch);
        } else {
            if (idle_cooldown > 0) idle_cooldown--;
            else _pick_task(ch);
        }
    } else if (!_target_still_valid(ch)) {
        _pick_task(ch);
    }

    if (!has_target_cell) {
        switch (state) {
            case STATE_TEND_BROOD:  _do_tend_brood(ch);  break;
            case STATE_TEND_QUEEN:  _do_tend_queen(ch);  break;
            case STATE_CANNIBALIZE: _do_cannibalize(ch); break;
            case STATE_TO_FOOD:     _do_to_food(ch);     break;
            case STATE_TO_HOME:     _do_to_home(ch);     break;
            default:                _do_idle(ch);        break;
        }
    }
}

// ================================================================
//  Movement engine
// ================================================================

bool LilGuy::_set_target_cell(int cx, int cy, Chamber& ch) {
    if (!ch.in_bounds(cx, cy)) return false;
    target_cell_x = cx;
    target_cell_y = cy;
    has_target_cell = true;
    return true;
}

void LilGuy::_advance_toward_target(Chamber& ch) {
    if (!has_target_cell) return;
    float tx = target_cell_x + 0.5f;
    float ty = target_cell_y + 0.5f;
    float dx = tx - x;
    float dy = ty - y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < Cfg::ARRIVAL_THRESHOLD) {
        // Snap to center, mark arrival
        x = tx;
        y = ty;
        int ncx = target_cell_x, ncy = target_cell_y;
        if (ncx != last_cell_x || ncy != last_cell_y) {
            _on_enter_cell(ncx, ncy, ch);
            last_cell_x = ncx;
            last_cell_y = ncy;
        }
        has_target_cell = false;
        return;
    }
    // Move toward target, don't overshoot
    float step = (speed < dist) ? speed : dist;
    x += (dx / dist) * step;
    y += (dy / dist) * step;
    // Update facing from velocity
    facing_dx = dx / dist;
    facing_dy = dy / dist;
    last_dx = facing_dx;
    last_dy = facing_dy;
    // Check cell entry mid-transit
    int ncx = cell_x(), ncy = cell_y();
    if (ncx != last_cell_x || ncy != last_cell_y) {
        _on_enter_cell(ncx, ncy, ch);
        last_cell_x = ncx;
        last_cell_y = ncy;
    }
}

void LilGuy::_on_enter_cell(int cx, int cy, Chamber& ch) {
    if (state == STATE_TO_FOOD) {
        float intensity = Cfg::BASE_MARKER_INTENSITY * expf(-Cfg::MARKER_STEP_DECAY * steps_walked);
        ch.deposit_home(cx, cy, intensity);
        steps_walked++;
        chamber_steps++;
    } else if (state == STATE_TO_HOME) {
        if (food_carried > 0) {
            float intensity = Cfg::BASE_MARKER_INTENSITY * expf(-Cfg::MARKER_STEP_DECAY * steps_walked);
            ch.deposit_food(cx, cy, intensity);
        }
        steps_walked++;
    }
}

// ================================================================
//  Task selection
// ================================================================

void LilGuy::_pick_task(Chamber& ch) {
    speed = Cfg::ROLE_PARAMS[role].speed;
    idle_ticks_remaining = 0;

    if (food_carried > 0) {
        state = STATE_TO_HOME;
        has_target = false;
        has_target_cell = false;
        return;
    }
    if (!ch.has_queen) {
        state = STATE_TO_HOME;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        return;
    }

    auto* col = ch.colony;
    float pressure = col->food_pressure();
    float feed_per_visit = Cfg::LARVA_FOOD_PER_DAY / 10.0f;
    bool has_food = col->food_store >= feed_per_visit;

    // Famine override -- feed queen first (bypasses idle)
    if (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE
            && ch.queen_obj.alive
            && ch.queen_obj.hunger > Cfg::QUEEN_PRIORITY_HUNGER
            && has_food
            && _within_sense(ch.queen_obj.x, ch.queen_obj.y)) {
        state = STATE_TEND_QUEEN;
        target_x = ch.queen_obj.x; target_y = ch.queen_obj.y;
        has_target = true;
        has_target_cell = false;
        return;
    }

    // Idle budget — gates all non-critical tasks.
    // Returns 0 during famine or founding, so crisis paths still fire.
    float budget = _colony_idle_budget(ch);
    if (budget > 0 && g_rng.rand_float() < budget) {
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                               Cfg::IDLE_REST_MAX_TICKS);
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        if (ch._food_pile_index(cell_x(), cell_y()) >= 0) {
            idle_microstate = 1;
            speed = Cfg::IDLE_DRIFT_SPEED;
            idle_micro_ticks = g_rng.rand_int(Cfg::IDLE_MICROSTATE_MIN_TICKS,
                                               Cfg::IDLE_MICROSTATE_MAX_TICKS);
        } else {
            _pick_idle_microstate(ch);
        }
        return;
    }

    // === Only ~30% of workers reach task selection below ===

    // Normal queen feeding
    if (ch.queen_obj.alive && ch.queen_obj.needs_feeding() && has_food
            && _within_sense(ch.queen_obj.x, ch.queen_obj.y)) {
        state = STATE_TEND_QUEEN;
        target_x = ch.queen_obj.x; target_y = ch.queen_obj.y;
        has_target = true;
        has_target_cell = false;
        return;
    }

    // Severe famine -- cannibalism or skip brood tending
    if (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE) {
        if (pressure >= Cfg::BROOD_CANNIBALISM_PRESSURE
                && col->food_store < feed_per_visit
                && ch.cannibalism_cooldown <= 0) {
            int vi = _least_invested_larva(ch);
            if (vi >= 0) {
                state = STATE_CANNIBALIZE;
                target_x = ch.brood[vi].x; target_y = ch.brood[vi].y;
                has_target = true;
                has_target_cell = false;
                ch.cannibalism_cooldown = static_cast<int>(Cfg::CANNIBALISM_COOLDOWN_SECS * 8);
                return;
            }
        }
    } else {
        // Normal domestic -- feed larvae (maintain gatherer floor)
        int li = _nearest_hungry_larva(ch);
        if (li >= 0 && has_food) {
            int total_pop = col->population;
            float target_frac = col->target_gatherer_fraction();
            int min_gatherers = (2 > static_cast<int>(total_pop * target_frac))
                             ? 2 : static_cast<int>(total_pop * target_frac);
            if (col->gatherer_count >= min_gatherers) {
                state = STATE_TEND_BROOD;
                target_x = ch.brood[li].x; target_y = ch.brood[li].y;
                has_target = true;
                has_target_cell = false;
                return;
            }
        }
    }

    // Recruitment signal
    if (_detects_food_trail(ch)) {
        state = STATE_TO_FOOD;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        chamber_steps = 0;
        return;
    }

    // Gatherer regulation — wander briefly then retry
    float target_frac = col->target_gatherer_fraction();
    int total_pop = col->population;
    if (total_pop > 0
            && static_cast<float>(col->gatherer_count) / total_pop >= target_frac) {
        int cd = (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE)
               ? g_rng.rand_int(10, 30)
               : g_rng.rand_int(Cfg::IDLE_RECONSIDER_MIN, Cfg::IDLE_RECONSIDER_MAX);
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_cooldown = cd;
        return;
    }

    // Go gather
    state = STATE_TO_FOOD;
    has_target = false;
    has_target_cell = false;
    steps_walked = 0;
    chamber_steps = 0;
}

// ================================================================
//  TO_FOOD
// ================================================================

void LilGuy::_do_to_food(Chamber& ch) {
    // Scout patience -- give up and head home after too long.
    if (steps_walked > Cfg::SCOUT_PATIENCE_TICKS) {
        state = STATE_TO_HOME;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        facing_dx = -facing_dx;
        facing_dy = -facing_dy;
        return;
    }

    // Adjacent food?
    int8_t px, py;
    if (_food_pile_adjacent(ch, px, py)) {
        float taken = ch.take_food(px, py, carry_amount);
        if (taken > 0) {
            food_carried = taken;
            state = STATE_TO_HOME;
            has_target = false;
            has_target_cell = false;
            steps_walked = 0;
            facing_dx = -facing_dx;
            facing_dy = -facing_dy;
            return;
        }
    }

    // Movement decision
    int cx = cell_x(), cy = cell_y();
    int8_t fx, fy;
    if (ch.nearest_food_within(cx, cy, sense_radius, fx, fy)) {
        _step_toward_cell(fx, fy, ch);
        stall_ticks = 0;
    } else if (stall_ticks < Cfg::STALL_THRESHOLD_TICKS) {
        int8_t dx, dy;
        if (_sample_markers(ch, true, dx, dy)) {
            int mcx = cell_x(), mcy = cell_y();
            _set_target_cell(mcx + dx, mcy + dy, ch);
        } else {
            _explore_or_wander(ch);
        }
    } else {
        _explore_or_wander(ch);
    }

    // Stall counter
    {
        int scx = cell_x(), scy = cell_y();
        int8_t fx2, fy2;
        if (!ch.nearest_food_within(scx, scy, sense_radius, fx2, fy2)) {
            int8_t dx, dy;
            if (_sample_markers(ch, true, dx, dy))
                stall_ticks++;
            else
                stall_ticks = 0;
        }
    }
}

// ================================================================
//  TO_HOME
// ================================================================

void LilGuy::_do_to_home(Chamber& ch) {
    if (ch.has_queen) {
        int qx = ch.queen_obj.x, qy = ch.queen_obj.y;
        int cx = cell_x(), cy = cell_y();
        if (abs(qx - cx) + abs(qy - cy) <= 1) {
            Event ev; ev.type = EVT_FOOD_DELIVERED; ev.tick = ch.tick_num;
            ev.food_delivered = {static_cast<int8_t>(cx), static_cast<int8_t>(cy), food_carried};
            ch.emit(ev);
            ch.colony->food_store += food_carried;
            food_carried = 0.0f;
            ch.food_delivery_signal = 200;
            state = STATE_IDLE;
            has_target = false;
            has_target_cell = false;
            steps_walked = 0;
            {
                uint32_t wear_ms = static_cast<uint32_t>(g_rng.rand_float() * 0.2f * Cfg::SECS_PER_DAY * 1000.0f);
                if (born_at_ms > wear_ms) born_at_ms -= wear_ms;
                else born_at_ms = 0;
            }
            facing_dx = -facing_dx;
            facing_dy = -facing_dy;
            return;
        }
        _step_toward_cell(qx, qy, ch);
    } else {
        int8_t dx, dy;
        if (_sample_markers(ch, false, dx, dy)) {
            int cx = cell_x(), cy = cell_y();
            _set_target_cell(cx + dx, cy + dy, ch);
        } else if (ch.home_face >= 0) {
            Face hf = static_cast<Face>(ch.home_face);
            _step_toward_cell(Cfg::ENTRY_X[hf], Cfg::ENTRY_Y[hf], ch);
        } else {
            _persistent_forward_step(ch);
        }
    }
}

// ================================================================
//  Domestic tasks
// ================================================================

void LilGuy::_do_tend_brood(Chamber& ch) {
    if (!has_target) { state = STATE_IDLE; has_target_cell = false; return; }
    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) <= 1) {
        float feed_amt = Cfg::LARVA_FOOD_PER_DAY / 10.0f;
        if (ch.colony->food_store >= feed_amt) {
            for (int i = 0; i < ch.brood_count; i++) {
                auto& b = ch.brood[i];
                if (b.x == target_x && b.y == target_y
                        && b.stage == STAGE_LARVA) {
                    if (!b.needs_feeding()) break;
                    ch.colony->food_store -= feed_amt;
                    b.feed(feed_amt);
                    if (ch.event_bus) {
                        uint16_t pid = ch.event_bus->next_pair_id();
                        Event es; es.type = EVT_INTERACTION_STARTED; es.tick = ch.tick_num;
                        es.interaction_started = {pid, INTERACT_TENDING_YOUNG,
                            static_cast<uint8_t>(Cfg::GREETING_DURATION_TICKS)};
                        ch.emit(es);
                        Event ee; ee.type = EVT_INTERACTION_ENDED; ee.tick = ch.tick_num;
                        ee.interaction_ended = {pid};
                        ch.emit(ee);
                    }
                    break;
                }
            }
        }
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_cooldown = g_rng.rand_int(20, 40);
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

void LilGuy::_do_tend_queen(Chamber& ch) {
    if (!has_target) { state = STATE_IDLE; has_target_cell = false; return; }
    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) <= 1) {
        float feed_amt = Cfg::QUEEN_FOOD_PER_DAY / 10.0f;
        if (ch.colony->food_store >= feed_amt) {
            ch.colony->food_store -= feed_amt;
            ch.queen_obj.hunger = fmaxf(0.0f, ch.queen_obj.hunger - 0.1f);
            if (ch.event_bus) {
                uint16_t pid = ch.event_bus->next_pair_id();
                Event es; es.type = EVT_INTERACTION_STARTED; es.tick = ch.tick_num;
                es.interaction_started = {pid, INTERACT_TENDING_QUEEN,
                    static_cast<uint8_t>(Cfg::GREETING_DURATION_TICKS)};
                ch.emit(es);
                Event ee; ee.type = EVT_INTERACTION_ENDED; ee.tick = ch.tick_num;
                ee.interaction_ended = {pid};
                ch.emit(ee);
            }
        }
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_cooldown = g_rng.rand_int(20, 40);
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

void LilGuy::_do_idle(Chamber& ch) {
    // True rest: only drift needs a new target cell
    if (idle_ticks_remaining > 0) {
        if (idle_microstate == 1) {
            int cx = cell_x(), cy = cell_y();
            int dx = g_rng.rand_dir();
            int dy = g_rng.rand_dir();
            if (dx != 0 && dy != 0) {
                if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
            }
            if (dx == 0 && dy == 0) dx = g_rng.rand_sign();
            _set_target_cell(cx + dx, cy + dy, ch);
        }
        return;
    }

    // Wander (old behavior)
    int cx = cell_x(), cy = cell_y();
    int dx, dy;
    if (ch.has_queen && g_rng.rand_float() < 0.3f) {
        dx = (ch.queen_obj.x > cx) ? 1 : ((ch.queen_obj.x < cx) ? -1 : 0);
        dy = (ch.queen_obj.y > cy) ? 1 : ((ch.queen_obj.y < cy) ? -1 : 0);
    } else {
        dx = g_rng.rand_dir();
        dy = g_rng.rand_dir();
    }
    if (dx != 0 && dy != 0) {
        if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
    }
    if (dx == 0 && dy == 0) return;
    _set_target_cell(cx + dx, cy + dy, ch);
}

void LilGuy::_do_cannibalize(Chamber& ch) {
    if (!has_target) { state = STATE_IDLE; has_target_cell = false; return; }
    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) <= 1) {
        for (int i = 0; i < ch.brood_count; i++) {
            auto& b = ch.brood[i];
            if (b.x == target_x && b.y == target_y
                    && b.stage == STAGE_LARVA && b.alive()) {
                float recovered = b.food_invested * Cfg::BROOD_CANNIBALISM_RECOVERY;
                if (recovered < Cfg::BROOD_CANNIBALISM_MIN_PILE)
                    recovered = Cfg::BROOD_CANNIBALISM_MIN_PILE;
                ch.colony->food_store += recovered;
                { Event ev; ev.type = EVT_YOUNG_DIED; ev.tick = ch.tick_num;
                  ev.position = {b.x, b.y}; ch.emit(ev); }
                ch.remove_brood(i);
                break;
            }
        }
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

// ================================================================
//  Idle/rest
// ================================================================

void LilGuy::_tick_idle(Chamber& ch) {
    idle_ticks_remaining--;
    idle_micro_ticks--;

    // Timer expired → exit idle
    if (idle_ticks_remaining <= 0) {
        speed = Cfg::ROLE_PARAMS[role].speed;
        has_target_cell = false;
        _pick_task(ch);
        return;
    }

    // Periodic repoll for work
    idle_repoll_tick--;
    if (idle_repoll_tick <= 0) {
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;

        int16_t saved_remaining   = idle_ticks_remaining;
        int16_t saved_micro_ticks = idle_micro_ticks;
        uint8_t saved_microstate  = idle_microstate;
        float   saved_speed       = speed;

        _pick_task(ch);

        if (state == STATE_IDLE) {
            // No work found — stay in current rest period
            idle_ticks_remaining = saved_remaining;
            idle_micro_ticks     = saved_micro_ticks;
            idle_microstate      = saved_microstate;
            speed                = saved_speed;
        }
        return;
    }

    // Microstate cycling
    if (idle_micro_ticks <= 0) {
        _pick_idle_microstate(ch);
    }
}

void LilGuy::_pick_idle_microstate(Chamber& ch) {
    has_target_cell = false;

    float r = g_rng.rand_float();
    if (r < Cfg::IDLE_HOLD_WEIGHT) {
        idle_microstate = 0;
        speed = Cfg::ROLE_PARAMS[role].speed;
    } else if (r < Cfg::IDLE_HOLD_WEIGHT + Cfg::IDLE_DRIFT_WEIGHT) {
        idle_microstate = 1;
        speed = Cfg::IDLE_DRIFT_SPEED;
    } else {
        idle_microstate = 2;
        speed = Cfg::ROLE_PARAMS[role].speed;
        int d = g_rng.rand_int(0, 3);
        const float fdx[] = {1.0f, -1.0f, 0.0f, 0.0f};
        const float fdy[] = {0.0f, 0.0f, 1.0f, -1.0f};
        facing_dx = fdx[d];
        facing_dy = fdy[d];
    }
    idle_micro_ticks = g_rng.rand_int(Cfg::IDLE_MICROSTATE_MIN_TICKS,
                                       Cfg::IDLE_MICROSTATE_MAX_TICKS);
}

float LilGuy::_colony_idle_budget(Chamber& ch) {
    if (ch.colony->population < Cfg::COLONY_MIN_ACTIVE_FOR_IDLE) return 0.0f;
    if (ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE) return 0.0f;
    // Blend idle budget by night_factor: day=0.70, night=0.95
    float nf = g_tod.night_factor;
    return Cfg::IDLE_BUDGET_DAY + nf * (Cfg::IDLE_BUDGET_NIGHT - Cfg::IDLE_BUDGET_DAY);
}

// ================================================================
//  Task validity
// ================================================================

bool LilGuy::_target_still_valid(Chamber& ch) {
    if (state == STATE_IDLE || state == STATE_TO_FOOD
            || state == STATE_TO_HOME || state == STATE_CANNIBALIZE)
        return true;
    if (!has_target) return false;
    if (state == STATE_TEND_QUEEN) {
        return ch.has_queen && ch.queen_obj.alive && ch.queen_obj.needs_feeding();
    }
    if (state == STATE_TEND_BROOD) {
        for (int i = 0; i < ch.brood_count; i++) {
            auto& b = ch.brood[i];
            if (b.x == target_x && b.y == target_y
                    && b.stage == STAGE_LARVA && b.alive() && b.needs_feeding())
                return true;
        }
        return false;
    }
    return true;
}

// ================================================================
//  Marker sampling
// ================================================================

bool LilGuy::_sample_markers(Chamber& ch, bool use_food, int8_t& out_dx, int8_t& out_dy) {
    int cx = cell_x(), cy = cell_y();
    struct { int8_t dx, dy; float val; } nbrs[4];
    if (use_food) {
        nbrs[0] = { 1,  0, ch.pheromones.food(cx+1, cy)};
        nbrs[1] = {-1,  0, ch.pheromones.food(cx-1, cy)};
        nbrs[2] = { 0,  1, ch.pheromones.food(cx, cy+1)};
        nbrs[3] = { 0, -1, ch.pheromones.food(cx, cy-1)};
    } else {
        nbrs[0] = { 1,  0, ch.pheromones.home(cx+1, cy)};
        nbrs[1] = {-1,  0, ch.pheromones.home(cx-1, cy)};
        nbrs[2] = { 0,  1, ch.pheromones.home(cx, cy+1)};
        nbrs[3] = { 0, -1, ch.pheromones.home(cx, cy-1)};
    }

    float best_val = 0.0f;
    int8_t best_dx[4], best_dy[4];
    int best_count = 0;

    for (int i = 0; i < 4; i++) {
        if (nbrs[i].val > best_val) {
            best_val = nbrs[i].val;
            best_dx[0] = nbrs[i].dx; best_dy[0] = nbrs[i].dy;
            best_count = 1;
        } else if (nbrs[i].val == best_val && nbrs[i].val > 0) {
            best_dx[best_count] = nbrs[i].dx;
            best_dy[best_count] = nbrs[i].dy;
            best_count++;
        }
    }

    if (best_val <= 0) return false;
    if (best_count == 1) {
        out_dx = best_dx[0]; out_dy = best_dy[0]; return true;
    }

    // Quantize facing to cardinal for tie-break
    int8_t fq_dx, fq_dy;
    if (fabsf(facing_dx) >= fabsf(facing_dy)) {
        fq_dx = (facing_dx > 0) ? 1 : -1; fq_dy = 0;
    } else {
        fq_dx = 0; fq_dy = (facing_dy > 0) ? 1 : -1;
    }
    for (int i = 0; i < best_count; i++) {
        if (best_dx[i] == fq_dx && best_dy[i] == fq_dy) {
            out_dx = fq_dx; out_dy = fq_dy; return true;
        }
    }
    int pick = g_rng.rand_int(0, best_count - 1);
    out_dx = best_dx[pick]; out_dy = best_dy[pick];
    return true;
}

// ================================================================
//  Movement helpers
// ================================================================

void LilGuy::_step_toward_cell(int tx, int ty, Chamber& ch) {
    int cx = cell_x(), cy = cell_y();
    if (cx == tx && cy == ty) return;
    int dx = (tx > cx) ? 1 : ((tx < cx) ? -1 : 0);
    int dy = (ty > cy) ? 1 : ((ty < cy) ? -1 : 0);

    if (abs(tx - cx) >= abs(ty - cy)) {
        if (!_set_target_cell(cx + dx, cy, ch))
            _set_target_cell(cx, cy + dy, ch);
    } else {
        if (!_set_target_cell(cx, cy + dy, ch))
            _set_target_cell(cx + dx, cy, ch);
    }
}

void LilGuy::_persistent_forward_step(Chamber& ch) {
    // Quantize facing to cardinal
    int fx, fy;
    if (fabsf(facing_dx) >= fabsf(facing_dy)) {
        fx = (facing_dx > 0) ? 1 : -1; fy = 0;
    } else {
        fx = 0; fy = (facing_dy > 0) ? 1 : -1;
    }

    float r = g_rng.rand_float();
    int dx, dy;
    if (r < 0.70f)       { dx = fx;  dy = fy; }
    else if (r < 0.82f)  { dx = -fy; dy = fx; }   // left
    else if (r < 0.94f)  { dx = fy;  dy = -fx; }  // right
    else                 { dx = -fx; dy = -fy; }  // reverse

    int cx = cell_x(), cy = cell_y();
    if (!ch.in_bounds(cx + dx, cy + dy)) {
        int alts[][2] = {{fx,fy}, {-fy,fx}, {fy,-fx}, {-fx,-fy}};
        for (auto& a : alts) {
            if (ch.in_bounds(cx + a[0], cy + a[1])) {
                dx = a[0]; dy = a[1]; break;
            }
        }
    }
    _set_target_cell(cx + dx, cy + dy, ch);
}

void LilGuy::_explore_or_wander(Chamber& ch) {
    if (chamber_steps >= Cfg::CHAMBER_EXPLORE_STEPS) {
        int8_t ex, ey;
        if (_nearest_active_entry(ch, 200, -1, ex, ey)) {
            _step_toward_cell(ex, ey, ch);
            return;
        }
    } else if (chamber_steps < 5 && ch.home_face >= 0) {
        // Just entered a non-home chamber -- push inward.
        // Quantize facing to cardinal for the push direction.
        int dx, dy;
        if (fabsf(facing_dx) >= fabsf(facing_dy)) {
            dx = (facing_dx > 0) ? 1 : -1; dy = 0;
        } else {
            dx = 0; dy = (facing_dy > 0) ? 1 : -1;
        }
        int cx = cell_x(), cy = cell_y();
        if (!_set_target_cell(cx + dx, cy + dy, ch))
            _persistent_forward_step(ch);
        return;
    } else if (g_rng.rand_float() < 0.3f) {
        int8_t ex, ey;
        if (_nearest_active_entry(ch, Cfg::ENTRY_ATTRACT_RADIUS, ch.home_face, ex, ey)) {
            _step_toward_cell(ex, ey, ch);
            return;
        }
    }
    _persistent_forward_step(ch);
}

// ================================================================
//  Query helpers
// ================================================================

bool LilGuy::_detects_food_trail(Chamber& ch) {
    return ch.food_delivery_signal > 0;
}

int LilGuy::_nearest_hungry_larva(Chamber& ch) {
    int cx = cell_x(), cy = cell_y();
    int best = -1;
    int best_d = 1000000;
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_LARVA || !b.alive() || !b.needs_feeding())
            continue;
        int d = abs(b.x - cx) + abs(b.y - cy);
        if (d <= sense_radius && d < best_d) {
            best = i; best_d = d;
        }
    }
    return best;
}

int LilGuy::_least_invested_larva(Chamber& ch) {
    int best = -1;
    float best_fed = 1000000.0f;
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_LARVA || !b.alive()) continue;
        if (b.food_invested < best_fed) { best = i; best_fed = b.food_invested; }
    }
    return best;
}

bool LilGuy::_food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y) {
    int cx = cell_x(), cy = cell_y();
    int idx = ch._food_pile_index(cx, cy);
    if (idx >= 0) { out_x = cx; out_y = cy; return true; }
    const int ddx[] = {1, -1, 0, 0};
    const int ddy[] = {0, 0, 1, -1};
    for (int i = 0; i < 4; i++) {
        int nx = cx + ddx[i], ny = cy + ddy[i];
        idx = ch._food_pile_index(nx, ny);
        if (idx >= 0 && ch.food_piles[idx].amount > 0) {
            out_x = nx; out_y = ny; return true;
        }
    }
    return false;
}

bool LilGuy::_nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
                                int8_t& out_x, int8_t& out_y) {
    int cx = cell_x(), cy = cell_y();
    int best_d = max_dist + 1;
    bool found = false;
    for (int f = 0; f < FACE_COUNT; f++) {
        if (ch.entries[f] < 0) continue;
        if (f == exclude_face) continue;
        int d = abs(Cfg::ENTRY_X[f] - cx) + abs(Cfg::ENTRY_Y[f] - cy);
        if (d < best_d) {
            best_d = d;
            out_x = Cfg::ENTRY_X[f]; out_y = Cfg::ENTRY_Y[f];
            found = true;
        }
    }
    return found;
}
