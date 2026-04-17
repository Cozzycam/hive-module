/* Worker lil_guy — full behavior port. */
#include "lil_guy.h"
#include "chamber.h"
#include "rng.h"
#include <cmath>

void LilGuy::init(int8_t px, int8_t py, Role c, bool pioneer) {
    x = px; y = py; prev_x = px; prev_y = py;
    state = STATE_IDLE;
    has_target = false;
    move_cooldown = 0;
    age = 0;
    alive = true;

    facing_dx = g_rng.rand_sign();
    facing_dy = 0;
    last_dx = facing_dx;
    last_dy = 0;
    food_carried = 0.0f;
    steps_walked = 0;
    ticks_away = 0;
    stall_ticks = 0;
    idle_cooldown = 0;
    chamber_steps = 0;
    hunger = 0.0f;

    role = c;
    is_pioneer = pioneer;
    const auto& p = Cfg::ROLE_PARAMS[c];
    move_ticks   = p.move_ticks;
    sense_radius = p.sense_radius;
    carry_amount = p.carry_amount;
    metabolism   = p.metabolism;

    if (pioneer)
        max_age = g_rng.rand_int(p.lifespan_pioneer_lo, p.lifespan_pioneer_hi);
    else
        max_age = g_rng.rand_int(p.lifespan_lo, p.lifespan_hi);
}

void LilGuy::tick(Chamber& ch) {
    if (!alive) return;
    age++;

    if (age >= max_age) { alive = false; return; }

    // Metabolism
    float scale = Cfg::metabolic_scale_factor(ch.colony->population);
    float drain = metabolism * scale;
    if (ch.colony->food_store >= drain) {
        ch.colony->food_store -= drain;
        if (hunger > 0) hunger = fmaxf(0.0f, hunger - drain);
    } else if (food_carried >= drain) {
        food_carried -= drain;
        if (hunger > 0) hunger = fmaxf(0.0f, hunger - drain);
    } else {
        hunger += Cfg::WORKER_HUNGER_RATE;
        if (hunger >= Cfg::WORKER_STARVE_THRESHOLD) { alive = false; return; }
    }

    // Track time away from queen chamber
    if (ch.has_queen) ticks_away = 0;
    else ticks_away++;

    // Return-home timer
    if (ticks_away >= Cfg::RETURN_HOME_TICKS && state != STATE_TO_HOME) {
        state = STATE_TO_HOME;
        has_target = false;
        steps_walked = 0;
        facing_dx = -facing_dx;
        facing_dy = -facing_dy;
    }

    // Task selection
    if (state == STATE_IDLE) {
        if (idle_cooldown > 0) idle_cooldown--;
        else _pick_task(ch);
    } else if (!_target_still_valid(ch)) {
        _pick_task(ch);
    }

    // Execute current state
    switch (state) {
        case STATE_TEND_BROOD:  _do_tend_brood(ch);  break;
        case STATE_TEND_QUEEN:  _do_tend_queen(ch);  break;
        case STATE_CANNIBALIZE: _do_cannibalize(ch); break;
        case STATE_TO_FOOD:     _do_to_food(ch);     break;
        case STATE_TO_HOME:     _do_to_home(ch);     break;
        default:                _do_idle(ch);        break;
    }
}

// ================================================================
//  Task selection
// ================================================================

void LilGuy::_pick_task(Chamber& ch) {
    if (food_carried > 0) {
        state = STATE_TO_HOME; has_target = false; return;
    }
    if (!ch.has_queen) {
        state = STATE_TO_HOME; has_target = false; steps_walked = 0; return;
    }

    auto* col = ch.colony;
    float pressure = col->food_pressure();
    bool has_food = col->food_store >= Cfg::LARVA_FEED_AMOUNT;

    // Famine override — feed queen first
    if (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE
            && ch.queen_obj.alive
            && ch.queen_obj.hunger > Cfg::QUEEN_PRIORITY_HUNGER
            && has_food
            && _within_sense(ch.queen_obj.x, ch.queen_obj.y)) {
        state = STATE_TEND_QUEEN;
        target_x = ch.queen_obj.x; target_y = ch.queen_obj.y;
        has_target = true; return;
    }

    // Normal queen feeding
    if (ch.queen_obj.alive && ch.queen_obj.needs_feeding() && has_food
            && _within_sense(ch.queen_obj.x, ch.queen_obj.y)) {
        state = STATE_TEND_QUEEN;
        target_x = ch.queen_obj.x; target_y = ch.queen_obj.y;
        has_target = true; return;
    }

    // Severe famine — cannibalism or skip brood tending
    if (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE) {
        if (pressure >= Cfg::BROOD_CANNIBALISM_PRESSURE
                && col->food_store < Cfg::LARVA_FEED_AMOUNT
                && ch.cannibalism_cooldown <= 0) {
            int vi = _least_invested_larva(ch);
            if (vi >= 0) {
                state = STATE_CANNIBALIZE;
                target_x = ch.brood[vi].x; target_y = ch.brood[vi].y;
                has_target = true;
                ch.cannibalism_cooldown = Cfg::BROOD_CANNIBALISM_COOLDOWN;
                return;
            }
        }
    } else {
        // Normal domestic — feed larvae (maintain gatherer floor)
        int li = _nearest_hungry_larva(ch);
        if (li >= 0 && has_food) {
            int total_pop = col->population;
            float target_frac = col->target_gatherer_fraction();
            int min_gatherers = (2 > static_cast<int>(total_pop * target_frac))
                             ? 2 : static_cast<int>(total_pop * target_frac);
            if (col->gatherer_count >= min_gatherers) {
                state = STATE_TEND_BROOD;
                target_x = ch.brood[li].x; target_y = ch.brood[li].y;
                has_target = true; return;
            }
        }
    }

    // Recruitment signal
    if (_detects_food_trail(ch)) {
        state = STATE_TO_FOOD;
        has_target = false; steps_walked = 0; chamber_steps = 0;
        return;
    }

    // Gatherer regulation
    float target_frac = col->target_gatherer_fraction();
    int total_pop = col->population;
    if (total_pop > 0
            && static_cast<float>(col->gatherer_count) / total_pop >= target_frac) {
        int cd = (pressure > Cfg::FAMINE_SLOWDOWN_PRESSURE)
               ? g_rng.rand_int(10, 30)
               : g_rng.rand_int(Cfg::IDLE_RECONSIDER_MIN, Cfg::IDLE_RECONSIDER_MAX);
        state = STATE_IDLE;
        has_target = false;
        idle_cooldown = cd;
        return;
    }

    // Go gather
    state = STATE_TO_FOOD;
    has_target = false; steps_walked = 0; chamber_steps = 0;
}

// ================================================================
//  TO_FOOD
// ================================================================

void LilGuy::_do_to_food(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (steps_walked > Cfg::SCOUT_PATIENCE_TICKS) {
        state = STATE_TO_HOME; has_target = false;
        steps_walked = 0;
        facing_dx = -facing_dx; facing_dy = -facing_dy;
        return;
    }

    // Adjacent food?
    int8_t px, py;
    if (_food_pile_adjacent(ch, px, py)) {
        float taken = ch.take_food(px, py, carry_amount);
        if (taken > 0) {
            food_carried = taken;
            state = STATE_TO_HOME; has_target = false;
            steps_walked = 0;
            facing_dx = -facing_dx; facing_dy = -facing_dy;
            return;
        }
    }

    // Movement decision
    int8_t fx, fy;
    if (ch.nearest_food_within(x, y, sense_radius, fx, fy)) {
        _step_toward_cell(fx, fy, ch);
        stall_ticks = 0;
    } else if (stall_ticks < Cfg::STALL_THRESHOLD_TICKS) {
        int8_t dx, dy;
        if (_sample_markers(ch, true, dx, dy))
            _try_move(dx, dy, ch);
        else
            _explore_or_wander(ch);
    } else {
        _explore_or_wander(ch);
    }

    // Stall counter
    {
        int8_t fx2, fy2;
        if (!ch.nearest_food_within(x, y, sense_radius, fx2, fy2)) {
            int8_t dx, dy;
            if (_sample_markers(ch, true, dx, dy))
                stall_ticks++;
            else
                stall_ticks = 0;
        }
    }

    // Deposit to_home marker
    float intensity = Cfg::BASE_MARKER_INTENSITY * expf(-Cfg::MARKER_STEP_DECAY * steps_walked);
    ch.deposit_home(x, y, intensity);
    steps_walked++;
    chamber_steps++;
}

// ================================================================
//  TO_HOME
// ================================================================

void LilGuy::_do_to_home(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (ch.has_queen) {
        int qx = ch.queen_obj.x, qy = ch.queen_obj.y;
        if (abs(qx - x) + abs(qy - y) <= 1) {
            ch.colony->food_store += food_carried;
            food_carried = 0.0f;
            ch.food_delivery_signal = 200;
            state = STATE_IDLE; has_target = false;
            steps_walked = 0;
            age += g_rng.rand_int(Cfg::GATHERING_TRIP_WEAR_LO, Cfg::GATHERING_TRIP_WEAR_HI);
            facing_dx = -facing_dx; facing_dy = -facing_dy;
            return;
        }
        _step_toward_cell(qx, qy, ch);
    } else {
        int8_t dx, dy;
        if (_sample_markers(ch, false, dx, dy)) {
            _try_move(dx, dy, ch);
        } else if (ch.home_face >= 0) {
            Face hf = static_cast<Face>(ch.home_face);
            _step_toward_cell(Cfg::ENTRY_X[hf], Cfg::ENTRY_Y[hf], ch);
        } else {
            _persistent_forward_step(ch);
        }
    }

    // Deposit to_food trail
    if (food_carried > 0) {
        float intensity = Cfg::BASE_MARKER_INTENSITY * expf(-Cfg::MARKER_STEP_DECAY * steps_walked);
        ch.deposit_food(x, y, intensity);
    }
    steps_walked++;
}

// ================================================================
//  Domestic tasks
// ================================================================

void LilGuy::_do_tend_brood(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (!has_target) { state = STATE_IDLE; return; }
    if (abs(target_x - x) + abs(target_y - y) <= 1) {
        float feed_amt = Cfg::LARVA_FEED_AMOUNT;
        if (ch.colony->food_store >= feed_amt) {
            for (int i = 0; i < ch.brood_count; i++) {
                auto& b = ch.brood[i];
                if (b.x == target_x && b.y == target_y
                        && b.stage == STAGE_LARVA) {
                    if (!b.needs_feeding()) break;
                    ch.colony->food_store -= feed_amt;
                    b.feed(feed_amt);
                    break;
                }
            }
        }
        state = STATE_IDLE; has_target = false;
        idle_cooldown = g_rng.rand_int(20, 40);
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

void LilGuy::_do_tend_queen(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (!has_target) { state = STATE_IDLE; return; }
    if (abs(target_x - x) + abs(target_y - y) <= 1) {
        float feed_amt = Cfg::LARVA_FEED_AMOUNT;
        if (ch.colony->food_store >= feed_amt) {
            ch.colony->food_store -= feed_amt;
            ch.queen_obj.hunger = fmaxf(0.0f, ch.queen_obj.hunger - feed_amt);
        }
        state = STATE_IDLE; has_target = false;
        idle_cooldown = g_rng.rand_int(20, 40);
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

void LilGuy::_do_idle(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (ch.has_queen && g_rng.rand_float() < 0.3f) {
        int dx = (ch.queen_obj.x > x) ? 1 : ((ch.queen_obj.x < x) ? -1 : 0);
        int dy = (ch.queen_obj.y > y) ? 1 : ((ch.queen_obj.y < y) ? -1 : 0);
        _try_move(dx, dy, ch);
    } else {
        _try_move(g_rng.rand_dir(), g_rng.rand_dir(), ch);
    }
}

void LilGuy::_do_cannibalize(Chamber& ch) {
    if (move_cooldown > 0) { move_cooldown--; return; }
    move_cooldown = _move_delay(ch);

    if (!has_target) { state = STATE_IDLE; return; }
    if (abs(target_x - x) + abs(target_y - y) <= 1) {
        for (int i = 0; i < ch.brood_count; i++) {
            auto& b = ch.brood[i];
            if (b.x == target_x && b.y == target_y
                    && b.stage == STAGE_LARVA && b.alive()) {
                float recovered = b.fed_total * Cfg::BROOD_CANNIBALISM_RECOVERY;
                if (recovered < Cfg::BROOD_CANNIBALISM_MIN_PILE)
                    recovered = Cfg::BROOD_CANNIBALISM_MIN_PILE;
                ch.colony->food_store += recovered;
                ch.remove_brood(i);
                break;
            }
        }
        state = STATE_IDLE; has_target = false;
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
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
    struct { int8_t dx, dy; float val; } nbrs[4];
    if (use_food) {
        nbrs[0] = { 1,  0, ch.pheromones.food(x+1, y)};
        nbrs[1] = {-1,  0, ch.pheromones.food(x-1, y)};
        nbrs[2] = { 0,  1, ch.pheromones.food(x, y+1)};
        nbrs[3] = { 0, -1, ch.pheromones.food(x, y-1)};
    } else {
        nbrs[0] = { 1,  0, ch.pheromones.home(x+1, y)};
        nbrs[1] = {-1,  0, ch.pheromones.home(x-1, y)};
        nbrs[2] = { 0,  1, ch.pheromones.home(x, y+1)};
        nbrs[3] = { 0, -1, ch.pheromones.home(x, y-1)};
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
    // Prefer facing direction
    for (int i = 0; i < best_count; i++) {
        if (best_dx[i] == facing_dx && best_dy[i] == facing_dy) {
            out_dx = facing_dx; out_dy = facing_dy; return true;
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
    if (x == tx && y == ty) return;
    int dx = (tx > x) ? 1 : ((tx < x) ? -1 : 0);
    int dy = (ty > y) ? 1 : ((ty < y) ? -1 : 0);

    if (abs(tx - x) >= abs(ty - y)) {
        if (!_try_move(dx, 0, ch)) _try_move(0, dy, ch);
    } else {
        if (!_try_move(0, dy, ch)) _try_move(dx, 0, ch);
    }
}

void LilGuy::_persistent_forward_step(Chamber& ch) {
    int fx = facing_dx, fy = facing_dy;
    if (fx == 0 && fy == 0) { fx = 1; fy = 0; }

    float r = g_rng.rand_float();
    int dx, dy;
    if (r < 0.70f)       { dx = fx;  dy = fy; }
    else if (r < 0.82f)  { dx = -fy; dy = fx; }   // left
    else if (r < 0.94f)  { dx = fy;  dy = -fx; }  // right
    else                 { dx = -fx; dy = -fy; }  // reverse

    if (!ch.in_bounds(x + dx, y + dy)) {
        int alts[][2] = {{fx,fy}, {-fy,fx}, {fy,-fx}, {-fx,-fy}};
        for (auto& a : alts) {
            if (ch.in_bounds(x + a[0], y + a[1])) {
                dx = a[0]; dy = a[1]; break;
            }
        }
    }
    _try_move(dx, dy, ch);
}

void LilGuy::_explore_or_wander(Chamber& ch) {
    if (chamber_steps >= Cfg::CHAMBER_EXPLORE_STEPS) {
        int8_t ex, ey;
        if (_nearest_active_entry(ch, 200, -1, ex, ey)) {
            _step_toward_cell(ex, ey, ch);
            return;
        }
    } else if (chamber_steps < 5 && ch.home_face >= 0) {
        int dx = facing_dx, dy = facing_dy;
        if (dx == 0 && dy == 0) { dx = 1; dy = 0; }
        if (!_try_move(dx, dy, ch))
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

bool LilGuy::_try_move(int dx, int dy, Chamber& ch) {
    if (dx != 0 && dy != 0) {
        if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
    }
    if (dx == 0 && dy == 0) return false;
    int nx = x + dx, ny = y + dy;
    if (!ch.in_bounds(nx, ny)) return false;
    x = nx; y = ny;
    facing_dx = dx; facing_dy = dy;
    last_dx = dx; last_dy = dy;
    return true;
}

int LilGuy::_move_delay(Chamber& ch) {
    if (ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE
            && state != STATE_TO_FOOD && state != STATE_TO_HOME)
        return move_ticks * 2;
    return move_ticks;
}

// ================================================================
//  Query helpers
// ================================================================

bool LilGuy::_detects_food_trail(Chamber& ch) {
    return ch.food_delivery_signal > 0;
}

int LilGuy::_nearest_hungry_larva(Chamber& ch) {
    int best = -1;
    int best_d = 1000000;
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_LARVA || !b.alive() || !b.needs_feeding())
            continue;
        int d = abs(b.x - x) + abs(b.y - y);
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
        if (b.fed_total < best_fed) { best = i; best_fed = b.fed_total; }
    }
    return best;
}

bool LilGuy::_food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y) {
    int idx = ch._food_pile_index(x, y);
    if (idx >= 0) { out_x = x; out_y = y; return true; }
    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};
    for (int i = 0; i < 4; i++) {
        int nx = x + dx[i], ny = y + dy[i];
        idx = ch._food_pile_index(nx, ny);
        if (idx >= 0 && ch.food_piles[idx].amount > 0) {
            out_x = nx; out_y = ny; return true;
        }
    }
    return false;
}

bool LilGuy::_nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
                                int8_t& out_x, int8_t& out_y) {
    int best_d = max_dist + 1;
    bool found = false;
    for (int f = 0; f < FACE_COUNT; f++) {
        if (ch.entries[f] < 0) continue;
        if (f == exclude_face) continue;
        int d = abs(Cfg::ENTRY_X[f] - x) + abs(Cfg::ENTRY_Y[f] - y);
        if (d < best_d) {
            best_d = d;
            out_x = Cfg::ENTRY_X[f]; out_y = Cfg::ENTRY_Y[f];
            found = true;
        }
    }
    return found;
}
