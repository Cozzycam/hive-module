/* Worker conker -- full behavior port with smooth sub-cell movement. */
#include "conker.h"
#include "chamber.h"
#include "rng.h"
#include "time_of_day.h"
#include <Arduino.h>
#include <cmath>

void Conker::init(int8_t px, int8_t py, Role c, bool pioneer) {
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

    anim_type = LG_ANIM_NONE;
    anim_remaining_ticks = 0;
    interaction_cooldown = 0;
    stack_on = -1;
    stack_hop_remaining = 0;
    sleeping = false;
    sleep_until_ms = 0;
    sleep_cooldown_ms = 0;
    zoomie_target = -1;
    zoomie_ticks = 0;
    flair_kind = 0;
    flair_ticks = 0;
    flair_casts_used = 0;
    flair_ceremony_done = false;
    tint_seed = static_cast<uint8_t>(g_rng.rand_int(1, 255));
    arrival_face = -1;
    arrival_ms = 0;
    departing = false;
    depart_at_ms = 0;
    depart_face = -1;

    // Personality: centred random (triangular distribution around 0.5)
    for (int i = 0; i < PERS_COUNT; i++)
        personality[i] = (g_rng.rand_float() + g_rng.rand_float()) * 0.5f;
    personality[PERS_RESERVE] = 0.0f;  // reserved for Phase 4+

    role = ROLE_CONKER;
    is_pioneer = pioneer;  // legacy, always false for new conkers
    is_founder = false;
    lived_ms = 0;
    const auto& p = Cfg::ROLE_PARAMS[ROLE_CONKER];
    move_ticks   = p.move_ticks;
    sense_radius = p.sense_radius;
    carry_amount = p.carry_amount;
    speed        = p.speed;

    float lifespan_days = g_rng.rand_gaussian(Cfg::CONKER_LIFESPAN_MEAN, Cfg::CONKER_LIFESPAN_SD);
    if (lifespan_days < 1.0f) lifespan_days = 1.0f;
    lifespan_ms = static_cast<uint32_t>(lifespan_days * Cfg::SECS_PER_DAY * 1000.0f);

    // Per-conker size from bell curve
    scale_factor = g_rng.rand_gaussian(Cfg::CONKER_SCALE_MEAN, Cfg::CONKER_SCALE_SD);
    if (scale_factor < Cfg::CONKER_SCALE_MIN) scale_factor = Cfg::CONKER_SCALE_MIN;
    if (scale_factor > Cfg::CONKER_SCALE_MAX) scale_factor = Cfg::CONKER_SCALE_MAX;
}

// ================================================================
//  Per-tick entry point -- two-phase architecture
// ================================================================

void Conker::tick(Chamber& ch, float dt) {
    if (!alive) return;

    // Gather override: rush toward finger in a ring, lean when close
    if (ch.gather_active) {
        lived_ms += static_cast<uint32_t>(dt * 1000.0f);

        float target_x, target_y;
        if (ch.gather_is_exit) {
            // Heading to edge to cross modules — go straight to entry point
            target_x = ch.gather_x;
            target_y = ch.gather_y;
        } else {
            // Local gather — form a ring around the finger
            float ring_radius = 2.5f;
            float angle = (id * 2654435761u) / 4294967296.0f * 6.2832f;
            target_x = ch.gather_x + ring_radius * cosf(angle);
            target_y = ch.gather_y + ring_radius * sinf(angle);
            if (target_x < 0.5f) target_x = 0.5f;
            if (target_y < 0.5f) target_y = 0.5f;
            if (target_x > Cfg::GRID_WIDTH - 0.5f) target_x = Cfg::GRID_WIDTH - 0.5f;
            if (target_y > Cfg::GRID_HEIGHT - 0.5f) target_y = Cfg::GRID_HEIGHT - 0.5f;
        }

        float dx = target_x - x;
        float dy = target_y - y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > 0.3f) {
            float spd = speed * 3.0f * dt * 8.0f;
            if (spd > dist) spd = dist;
            x += (dx / dist) * spd;
            y += (dy / dist) * spd;
            facing_dx = dx / dist;
            facing_dy = dy / dist;
            last_dx = facing_dx;
            last_dy = facing_dy;
            anim_type = LG_ANIM_NONE;
        } else if (!ch.gather_is_exit) {
            // On the ring — lean toward the center (finger)
            float cdx = ch.gather_x - x;
            float cdy = ch.gather_y - y;
            anim_type = LG_ANIM_GROOMING;
            anim_remaining_ticks = 8;
            anim_lean_dx = (cdx > 0.1f) ? 1 : (cdx < -0.1f) ? -1 : 0;
            anim_lean_dy = (cdy > 0.1f) ? 1 : (cdy < -0.1f) ? -1 : 0;
        }
        // Exit gathers: don't clamp — let edge crossing detect the boundary position
        if (!ch.gather_is_exit) {
            if (x < 0.5f) x = 0.5f;
            if (y < 0.5f) y = 0.5f;
            if (x > Cfg::GRID_WIDTH - 0.5f) x = Cfg::GRID_WIDTH - 0.5f;
            if (y > Cfg::GRID_HEIGHT - 0.5f) y = Cfg::GRID_HEIGHT - 0.5f;
        }
        return;
    }

    // Accumulate lived time (only advances while sim is running)
    lived_ms += static_cast<uint32_t>(dt * 1000.0f);

    if (lived_ms >= lifespan_ms) {
        alive = false;
        if (ch.colony->worker_census > 0) ch.colony->worker_census--;
        extern uint16_t g_deaths_old_age;
        g_deaths_old_age++;
        Serial.printf("[death] OLD AGE hunger=%.0f state=%d\r\n", hunger, state);
        Event ev; ev.type = EVT_CONKER_DIED; ev.tick = ch.tick_num;
        ev.position = {static_cast<int8_t>(cell_x()), static_cast<int8_t>(cell_y())};
        ch.emit(ev);
        return;
    }

    // Hunger rises continuously — eating is the only way to reduce it
    // hardiness biases: high hardiness = slower hunger (0.8x to 1.2x rate)
    float hardiness_scale = 1.2f - 0.4f * personality[PERS_HARDINESS];
    hunger += Cfg::WORKER_HUNGER_PER_DAY / Cfg::SECS_PER_DAY * dt * hardiness_scale;
    if (hunger >= Cfg::HUNGER_STARVE) {
        alive = false;
        if (ch.colony->worker_census > 0) ch.colony->worker_census--;
        extern uint16_t g_deaths_starved;
        g_deaths_starved++;
        Serial.printf("[death] STARVED hunger=%.0f state=%d food=%.1f\r\n",
                      hunger, state, ch.colony->food_store);
        Event ev; ev.type = EVT_CONKER_DIED; ev.tick = ch.tick_num;
        ev.position = {static_cast<int8_t>(cell_x()), static_cast<int8_t>(cell_y())};
        ch.emit(ev);
        return;
    }

    // Speed: base speed modified by ageing and starvation
    float base_speed = Cfg::ROLE_PARAMS[ROLE_CONKER].speed;

    // Ageing slowdown: 50-100% of lifespan → ramp from 100% to 50% speed
    if (lifespan_ms > 0) {
        float age_frac = static_cast<float>(lived_ms) / lifespan_ms;
        if (age_frac > 0.5f) {
            float slow = (age_frac - 0.5f);  // 0.0 to 0.5
            base_speed *= (1.0f - slow);       // 100% to 50%
        }
    }

    speed = base_speed;

    // Forage tempo: personality flavours outbound travel speed only —
    // return trips stay uniform so trail timing/strength is unaffected
    if (state == STATE_TO_FOOD) {
        speed *= Cfg::FORAGE_TEMPO_SPEED_MIN
               + Cfg::FORAGE_TEMPO_SPEED_SPAN * personality[PERS_WORK_TEMPO];
    }

    // Zoomies sprint multiplier (re-applied here because speed resets each
    // tick). Firefly chases use a gentler night-dash.
    if (state == STATE_ZOOMIES) {
        speed *= (zoomie_target <= -2) ? Cfg::FIREFLY_CHASE_SPEED_MULT
                                       : Cfg::ZOOMIE_SPEED_MULT;
    }

    // Starvation penalty (80-100: linearly reduce to 30%)
    if (hunger > Cfg::HUNGER_SLOWDOWN) {
        float penalty = (hunger - Cfg::HUNGER_SLOWDOWN)
                      / (Cfg::HUNGER_STARVE - Cfg::HUNGER_SLOWDOWN);
        speed = base_speed * (1.0f - penalty * 0.7f);
    }

    // Track time away from queen chamber
    if (ch.has_queen) ticks_away = 0;
    else ticks_away++;

    // Return-home timer — crisis overrides animation
    if (ticks_away >= Cfg::RETURN_HOME_TICKS && state != STATE_TO_HOME) {
        state = STATE_TO_HOME;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        facing_dx = -facing_dx;
        facing_dy = -facing_dy;
        anim_type = LG_ANIM_NONE;
        anim_remaining_ticks = 0;
    }

    // Crisis state cancels animation
    if (state == STATE_CANNIBALIZE && anim_type != LG_ANIM_NONE) {
        anim_type = LG_ANIM_NONE;
        anim_remaining_ticks = 0;
    }

    // Tick cooldowns
    if (interaction_cooldown > 0) interaction_cooldown--;

    // Stacking: track the ant below, skip normal behavior unless idle expired
    if (stack_on >= 0) {
        auto& below = ch.conkers[stack_on];
        if (!below.alive) { stack_on = -1; sleeping = false; anim_type = LG_ANIM_NONE; }
        else {
            if (stack_hop_remaining > 0) {
                x += (below.x - x) * 0.3f;
                y += (below.y - y) * 0.3f;
                stack_hop_remaining--;
            } else {
                x = below.x; y = below.y;
            }
            // Sleeping ants show snooze sprite, skip grooming
            // Wake on famine or personal hunger crisis
            if (sleeping) {
                bool swake = (millis() >= sleep_until_ms)
                          || ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE
                          || hunger > 60.0f;
                if (swake) {
                    sleeping = false;
                    anim_type = LG_ANIM_NONE;
                    sleep_cooldown_ms = millis() + 4UL * 3600UL * 1000UL;
                    stack_on = -1;  // unstack to go hustle
                } else {
                    anim_type = LG_ANIM_SNOOZE;
                }
            } else if (anim_type == LG_ANIM_TOPPLE) {
                // Toppling — let the main animation handler deal with it
                // (fall through past the stacked block)
            } else {
                // Occasionally groom the ant below while stacked
                if (anim_remaining_ticks > 0) {
                    anim_remaining_ticks--;
                    if (anim_remaining_ticks == 0) anim_type = LG_ANIM_NONE;
                } else if (anim_type == LG_ANIM_NONE && g_rng.rand_float() < 0.03f) {
                    anim_type = LG_ANIM_GROOMING;
                    anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
                    anim_lean_dx = 0;
                    anim_lean_dy = 1;
                }
            }
            // Stay stacked while idle; only fall through when timer expires
            // so _pick_task can decide whether to unstack or stay idle
            if (anim_type != LG_ANIM_TOPPLE && idle_ticks_remaining > 0) {
                idle_ticks_remaining--;
                return;
            }
            // Timer expired — fall through to normal behavior
            // (_pick_task will unstack if it assigns real work)
        }
    }

    // Sleeping: stay put, skip normal behavior until wake time
    // Wake up early during famine or if starving
    if (sleeping) {
        bool wake = (millis() >= sleep_until_ms);
        if (!wake && (ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE
                      || hunger > 60.0f)) {
            wake = true;
        }
        if (wake) {
            sleeping = false;
            anim_type = LG_ANIM_NONE;
            sleep_cooldown_ms = millis() + 4UL * 3600UL * 1000UL;
        } else {
            anim_type = LG_ANIM_SNOOZE;
            return;
        }
    }

    // Animation freeze: skip movement and behavior dispatch while animating
    if (anim_remaining_ticks > 0) {
        anim_remaining_ticks--;
        // Topple: unstack at the wobble→fall transition
        if (anim_type == LG_ANIM_TOPPLE
            && anim_remaining_ticks == Cfg::STACK_FALL_TICKS) {
            stack_on = -1;
            sleeping = false;
            stack_cooldown_ms = millis() + Cfg::STACK_COLLAPSE_COOLDOWN_MS;
        }
        if (anim_remaining_ticks == 0) {
            // End of fall: nudge position to match scatter direction
            if (anim_type == LG_ANIM_TOPPLE && topple_depth > 0) {
                float nudge = (topple_depth & 1) ? -1.0f : 1.0f;
                float nx = x + nudge;
                if (nx >= 0 && nx < Cfg::GRID_WIDTH) x = nx;
            }
            anim_type = LG_ANIM_NONE;
            topple_depth = 0;
        }
        return;
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
            case STATE_EATING:      _do_eating(ch);      break;
            case STATE_ZOOMIES:     _do_zoomies(ch);     break;
            case STATE_MOURNING:    _do_mourning(ch);    break;
            default:                _do_idle(ch);        break;
        }
    }
}

// ================================================================
//  Movement engine
// ================================================================

bool Conker::_set_target_cell(int cx, int cy, Chamber& ch) {
    if (!ch.in_bounds(cx, cy)) return false;
    // Idle ants can't enter the queen's body cells (but can move out if already inside)
    if (state == STATE_IDLE && ch.has_queen && ch.queen_obj.alive) {
        bool dest_inside = abs(cx - ch.queen_obj.x) <= Cfg::QUEEN_BODY_HALF_W
                        && abs(cy - ch.queen_obj.y) <= Cfg::QUEEN_BODY_HALF_H;
        if (dest_inside) {
            bool already_inside = abs(cell_x() - ch.queen_obj.x) <= Cfg::QUEEN_BODY_HALF_W
                               && abs(cell_y() - ch.queen_obj.y) <= Cfg::QUEEN_BODY_HALF_H;
            if (!already_inside) return false;  // block entry
            // Already inside: only allow moves that get closer to the edge
            int cur_max = max(abs(cell_x() - ch.queen_obj.x), abs(cell_y() - ch.queen_obj.y));
            int new_max = max(abs(cx - ch.queen_obj.x), abs(cy - ch.queen_obj.y));
            if (new_max < cur_max) return false;  // moving deeper, block
        }
    }
    target_cell_x = cx;
    target_cell_y = cy;
    has_target_cell = true;
    return true;
}

void Conker::_advance_toward_target(Chamber& ch) {
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

void Conker::_on_enter_cell(int cx, int cy, Chamber& ch) {
    chamber_steps++;
    if (state == STATE_TO_FOOD) {
        steps_walked++;
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

void Conker::_pick_task(Chamber& ch) {
    speed = Cfg::ROLE_PARAMS[role].speed;
    idle_ticks_remaining = 0;
    zoomie_target = -1;
    zoomie_ticks = 0;
    flair_kind = 0;
    flair_ticks = 0;
    flair_casts_used = 0;
    flair_ceremony_done = false;
    int16_t was_stacked = stack_on;
    bool was_sleeping = sleeping;
    stack_on = -1;   // assume unstack; restore if we stay idle
    sleeping = false; // assume wake; restore if we stay idle

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

    // ---- Hunger-driven eating ----
    // Probability ramps with hunger level; at 61+ it's top priority
    {
        float eat_chance = 0.0f;
        if (hunger > 60.0f)      eat_chance = 1.0f;
        else if (hunger > 40.0f) eat_chance = 0.60f;
        else if (hunger > 20.0f) eat_chance = 0.15f;

        if (eat_chance > 0.0f && ch.has_queen
                && col->food_store >= Cfg::WORKER_MEAL_COST
                && g_rng.rand_float() < eat_chance) {
            state = STATE_EATING;
            int8_t fx, fy; ch.food_store_pos(fx, fy);
            target_x = fx; target_y = fy;
            has_target = true;
            has_target_cell = false;
            return;
        }
    }

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

    // Sleep check — skip during famine so some workers keep hustling
    if (!was_sleeping && !sleeping
            && g_tod.phase == PHASE_NIGHT
            && millis() >= sleep_cooldown_ms
            && pressure <= Cfg::FAMINE_SLOWDOWN_PRESSURE
            && g_rng.rand_float() < 0.25f) {
        stack_on = was_stacked;
        sleeping = true;
        sleep_until_ms = millis() + 3600UL * 1000UL;
        anim_type = LG_ANIM_SNOOZE;
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                               Cfg::IDLE_REST_MAX_TICKS);
        return;
    }

    // Idle budget — gates all non-critical tasks.
    // Returns 0 during famine or founding, so crisis paths still fire.
    // work_tempo biases: high tempo workers are less likely to idle
    float budget = _colony_idle_budget(ch);
    float tempo_bias = 1.3f - 0.6f * personality[PERS_WORK_TEMPO];  // 0.7 to 1.3
    if (budget > 0 && g_rng.rand_float() < budget * tempo_bias) {
        stack_on = was_stacked;
        sleeping = was_sleeping;
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        {
            float t_scale = 1.3f - 0.6f * personality[PERS_WORK_TEMPO];
            int rest = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS, Cfg::IDLE_REST_MAX_TICKS);
            idle_ticks_remaining = static_cast<int16_t>(rest * t_scale);
        }
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        if (was_stacked < 0 && !sleeping) {
            if (ch._food_pile_index(cell_x(), cell_y()) >= 0) {
                idle_microstate = 1;
                speed = Cfg::IDLE_DRIFT_SPEED;
                idle_micro_ticks = g_rng.rand_int(Cfg::IDLE_MICROSTATE_MIN_TICKS,
                                                   Cfg::IDLE_MICROSTATE_MAX_TICKS);
            } else {
                _pick_idle_microstate(ch);
            }
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

    // Forager cap — count current foragers vs desired fraction
    int max_foragers = _max_foragers(ch);

    if (col->gatherer_count < max_foragers) {
        // Go gather
        state = STATE_TO_FOOD;
        has_target = false;
        has_target_cell = false;
        steps_walked = 0;
        chamber_steps = 0;
        return;
    }

    // Non-forager fallthrough: tend queen > tend brood > idle
    if (ch.queen_obj.alive && ch.queen_obj.needs_feeding() && has_food
            && _within_sense(ch.queen_obj.x, ch.queen_obj.y)) {
        state = STATE_TEND_QUEEN;
        target_x = ch.queen_obj.x; target_y = ch.queen_obj.y;
        has_target = true;
        has_target_cell = false;
        return;
    }

    {
        int li = _nearest_hungry_larva(ch);
        if (li >= 0 && has_food) {
            state = STATE_TEND_BROOD;
            target_x = ch.brood[li].x; target_y = ch.brood[li].y;
            has_target = true;
            has_target_cell = false;
            return;
        }
    }

    // Nothing to tend — idle
    stack_on = was_stacked;
    sleeping = was_sleeping;
    state = STATE_IDLE;
    has_target = false;
    has_target_cell = false;
    idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                           Cfg::IDLE_REST_MAX_TICKS);
    idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
    if (was_stacked < 0 && !sleeping) _pick_idle_microstate(ch);
}

// ================================================================
//  TO_FOOD
// ================================================================

void Conker::_do_to_food(Chamber& ch) {
    // Periodic forager re-evaluation — recall excess scouts
    if (steps_walked > 0 && steps_walked % 32 == 0 && food_carried == 0) {
        if (ch.colony->gatherer_count > _max_foragers(ch)) {
            state = STATE_TO_HOME;
            has_target = false;
            has_target_cell = false;
            steps_walked = 0;
            facing_dx = -facing_dx;
            facing_dy = -facing_dy;
            return;
        }
    }

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

    // Flair hold: search-cast scanning or pile-inspect linger.
    // Pure pause — state stays TO_FOOD, recall/patience above still apply.
    if (flair_ticks > 0) {
        flair_ticks--;
        if (flair_kind == 1 && (flair_ticks % 3) == 0) {
            // Scan: alternate 90-degree refaces, like casting for a scent
            float fdx = facing_dx, fdy = facing_dy;
            if (((flair_ticks / 3) & 1) == 0) { facing_dx = -fdy; facing_dy = fdx; }
            else                              { facing_dx = fdy;  facing_dy = -fdx; }
            last_dx = facing_dx; last_dy = facing_dy;
        }
        if (flair_ticks == 0) {
            if (flair_kind == 1
                    && g_rng.rand_float() < personality[PERS_EXPLORATION]) {
                // Explorers commit to a fresh heading after scanning
                int d = g_rng.rand_int(0, 3);
                const float cdx[4] = {1.0f, -1.0f, 0.0f, 0.0f};
                const float cdy[4] = {0.0f, 0.0f, 1.0f, -1.0f};
                facing_dx = cdx[d]; facing_dy = cdy[d];
                last_dx = facing_dx; last_dy = facing_dy;
            }
            flair_kind = 0;
        }
        return;
    }

    // Adjacent food?
    int8_t px, py;
    if (_food_pile_adjacent(ch, px, py)) {
        // Pickup ceremony: inspect the pile before hefting (once per trip)
        if (!flair_ceremony_done && _flair_allowed(ch)) {
            flair_ceremony_done = true;
            anim_type = LG_ANIM_GROOMING;
            anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
            float bdx = (px + 0.5f) - x, bdy = (py + 0.5f) - y;
            if (fabsf(bdx) >= fabsf(bdy)) {
                anim_lean_dx = (bdx > 0) ? 1 : -1; anim_lean_dy = 0;
            } else {
                anim_lean_dx = 0; anim_lean_dy = (bdy > 0) ? 1 : -1;
            }
            // Picky conkers linger over the choice a moment longer
            int linger = static_cast<int>(personality[PERS_FOOD_PREFERENCE]
                                          * Cfg::CEREMONY_LINGER_MAX_TICKS);
            if (linger > 0) {
                flair_kind = 2;
                flair_ticks = static_cast<uint8_t>(linger);
            }
            return;
        }
        // Overload fumble: rare comic topple from biting off too much
        if (_flair_allowed(ch)
                && g_rng.rand_float() < Cfg::OVERLOAD_TOPPLE_CHANCE) {
            anim_type = LG_ANIM_TOPPLE;
            anim_remaining_ticks = Cfg::STACK_TOPPLE_TICKS + Cfg::STACK_FALL_TICKS;
            topple_depth = 1;  // ground-level wobble + tip-over scatter
            return;
        }
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
            _explore_with_flair(ch);
        }
    } else {
        _explore_with_flair(ch);
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

void Conker::_do_to_home(Chamber& ch) {
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
            if (_flair_allowed(ch)) {
                // Waggle dance: advertise the find. The recruitment itself
                // rides food_delivery_signal via _max_foragers — this is
                // the visible celebration.
                idle_ticks_remaining = Cfg::WAGGLE_DANCE_TICKS;
                idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
                idle_microstate = 4;
                idle_micro_ticks = Cfg::WAGGLE_DANCE_TICKS;
            }
            facing_dx = -facing_dx;
            facing_dy = -facing_dy;
            return;
        }
        _step_toward_cell(qx, qy, ch);
    } else {
        // No queen — head directly for exit (home markers from wandering
        // foragers create noise that disrupts the gradient on satellites)
        if (ch.home_face >= 0) {
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

void Conker::_do_tend_brood(Chamber& ch) {
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
                    // Lineage: first carer becomes tended_by
                    if (b.tended_by == 0 && id != 0)
                        b.tended_by = id;
                    // Grooming animation: lean toward brood
                    anim_type = LG_ANIM_GROOMING;
                    anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
                    float bdx = b.x - x, bdy = b.y - y;
                    if (fabsf(bdx) >= fabsf(bdy)) {
                        anim_lean_dx = (bdx > 0) ? 1 : -1; anim_lean_dy = 0;
                    } else {
                        anim_lean_dx = 0; anim_lean_dy = (bdy > 0) ? 1 : -1;
                    }
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

void Conker::_do_tend_queen(Chamber& ch) {
    if (!has_target) { state = STATE_IDLE; has_target_cell = false; return; }
    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) <= 1) {
        float feed_amt = Cfg::QUEEN_MEAL_COST;
        if (ch.colony->food_store >= feed_amt) {
            ch.colony->food_store -= feed_amt;
            ch.queen_obj.hunger = 0.0f;
            // Grooming animation: lean toward queen
            anim_type = LG_ANIM_GROOMING;
            anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
            float qdx = ch.queen_obj.x - x, qdy = ch.queen_obj.y - y;
            if (fabsf(qdx) >= fabsf(qdy)) {
                anim_lean_dx = (qdx > 0) ? 1 : -1; anim_lean_dy = 0;
            } else {
                anim_lean_dx = 0; anim_lean_dy = (qdy > 0) ? 1 : -1;
            }
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
        // Done tending — re-evaluate tasks (may forage, tend brood, etc.)
        has_target = false;
        has_target_cell = false;
        _pick_task(ch);
        return;
    }
    _step_toward_cell(target_x, target_y, ch);
}

void Conker::_do_eating(Chamber& ch) {
    if (!ch.has_queen) {
        state = STATE_IDLE; has_target = false; has_target_cell = false;
        return;
    }
    int8_t fx, fy; ch.food_store_pos(fx, fy);
    int cx = cell_x(), cy = cell_y();
    if (abs(fx - cx) + abs(fy - cy) <= 1) {
        if (ch.colony->food_store >= Cfg::WORKER_MEAL_COST) {
            ch.colony->food_store -= Cfg::WORKER_MEAL_COST;
            hunger = 0.0f;
            speed = Cfg::ROLE_PARAMS[role].speed;  // clear any starvation penalty
            anim_type = LG_ANIM_GROOMING;
            anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
            float qdx = fx - x, qdy = fy - y;
            if (fabsf(qdx) >= fabsf(qdy)) {
                anim_lean_dx = (qdx > 0) ? 1 : -1; anim_lean_dy = 0;
            } else {
                anim_lean_dx = 0; anim_lean_dy = (qdy > 0) ? 1 : -1;
            }
        }
        has_target = false;
        has_target_cell = false;
        _pick_task(ch);
        return;
    }
    _step_toward_cell(fx, fy, ch);
}

void Conker::_do_idle(Chamber& ch) {
    // Escape queen exclusion zone: override everything, walk straight out
    if (ch.has_queen && ch.queen_obj.alive) {
        int cx = cell_x(), cy = cell_y();
        if (abs(cx - ch.queen_obj.x) <= Cfg::QUEEN_BODY_HALF_W
            && abs(cy - ch.queen_obj.y) <= Cfg::QUEEN_BODY_HALF_H) {
            int dx = (cx > ch.queen_obj.x) ? 1 : ((cx < ch.queen_obj.x) ? -1 : g_rng.rand_sign());
            int dy = (cy > ch.queen_obj.y) ? 1 : ((cy < ch.queen_obj.y) ? -1 : 0);
            if (dx != 0 && dy != 0) {
                if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
            }
            _set_target_cell(cx + dx, cy + dy, ch);
            return;
        }
    }

    // Time-of-day determines idle style:
    //   Day/Dawn  — wander freely, no queen bias
    //   Dusk      — drift toward queen, huddle near her
    //   Night     — huddle around queen, sleep
    bool queen_pull = ch.has_queen && ch.queen_obj.alive
                   && (g_tod.phase == PHASE_DUSK || g_tod.phase == PHASE_NIGHT);

    // True rest: drift and huddle need new target cells
    if (idle_ticks_remaining > 0) {
        if (idle_microstate == 4) {
            // Waggle dance: rapid alternating refaces in place
            if ((idle_ticks_remaining & 1) == 0) {
                float fdx = facing_dx, fdy = facing_dy;
                if (idle_ticks_remaining & 2) { facing_dx = -fdy; facing_dy = fdx; }
                else                          { facing_dx = fdy;  facing_dy = -fdx; }
                last_dx = facing_dx; last_dy = facing_dy;
            }
            return;
        }
        if (idle_microstate == 1) {
            // Drift — queen bias only at dusk/night
            int cx = cell_x(), cy = cell_y();
            int dx, dy;
            if (queen_pull && g_rng.rand_float() < 0.5f) {
                dx = (ch.queen_obj.x > cx) ? 1 : ((ch.queen_obj.x < cx) ? -1 : 0);
                dy = (ch.queen_obj.y > cy) ? 1 : ((ch.queen_obj.y < cy) ? -1 : 0);
            } else {
                dx = g_rng.rand_dir();
                dy = g_rng.rand_dir();
            }
            if (dx != 0 && dy != 0) {
                if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
            }
            if (dx == 0 && dy == 0) dx = g_rng.rand_sign();

            _set_target_cell(cx + dx, cy + dy, ch);
        } else if (idle_microstate == 3) {
            // Huddle: drift toward nearest idle neighbor (and queen at dusk/night)
            int cx = cell_x(), cy = cell_y();
            int best_dist = 999;
            int tx = cx, ty = cy;
            bool target_is_queen = false;

            // Find nearest idle worker — elders pull harder, so idle circles
            // form around the old ones (story time)
            for (int i = 0; i < ch.conker_count; i++) {
                auto& other = ch.conkers[i];
                if (&other == this || !other.alive) continue;
                if (other.state != STATE_IDLE) continue;
                int ox = other.cell_x(), oy = other.cell_y();
                int d = abs(ox - cx) + abs(oy - cy);
                if (other.is_elder()) d -= Cfg::ELDER_HUDDLE_PULL;
                if (d > 0 && d < best_dist) {
                    best_dist = d;
                    tx = ox; ty = oy;
                }
            }

            // Queen as huddle target: always available, stronger pull at dusk/night
            if (ch.has_queen && ch.queen_obj.alive) {
                float queen_chance = queen_pull ? 0.4f : 0.2f;
                int d = abs(ch.queen_obj.x - cx) + abs(ch.queen_obj.y - cy);
                if (d > 0 && (d < best_dist || g_rng.rand_float() < queen_chance)) {
                    tx = ch.queen_obj.x; ty = ch.queen_obj.y;
                    best_dist = d;
                    target_is_queen = true;
                }
            }

            // Close enough? Orbit tangentially instead of piling on
            int comfort = target_is_queen ? (Cfg::QUEEN_BODY_HALF_W + 1) : 2;
            if (best_dist <= comfort) {
                int dx = (tx > cx) ? 1 : ((tx < cx) ? -1 : 0);
                int dy = (ty > cy) ? 1 : ((ty < cy) ? -1 : 0);
                int tdx, tdy;
                if (g_rng.rand_float() < 0.5f) { tdx = -dy; tdy = dx; }
                else                            { tdx = dy;  tdy = -dx; }
                if (tdx == 0 && tdy == 0) tdx = g_rng.rand_sign();
                _set_target_cell(cx + tdx, cy + tdy, ch);
            } else {
                int dx = (tx > cx) ? 1 : ((tx < cx) ? -1 : 0);
                int dy = (ty > cy) ? 1 : ((ty < cy) ? -1 : 0);
                if (dx != 0 && dy != 0) {
                    if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0;
                }
                if (dx == 0 && dy == 0) dx = g_rng.rand_sign();
                _set_target_cell(cx + dx, cy + dy, ch);
            }
        }
        return;
    }

    // Wander (non-resting idle, legacy path)
    int cx = cell_x(), cy = cell_y();
    int dx, dy;
    if (queen_pull && g_rng.rand_float() < 0.5f) {
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

void Conker::_do_cannibalize(Chamber& ch) {
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
//  Zoomies (daytime chase)
// ================================================================

void Conker::_do_zoomies(Chamber& ch) {
    zoomie_ticks--;

    // End conditions: timer expired, chaser's target invalid
    bool done = (zoomie_ticks <= 0);
    if (!done && zoomie_target >= 0
        && (zoomie_target >= ch.conker_count
            || !ch.conkers[zoomie_target].alive)) {
        done = true;
    }
    if (!done && zoomie_target <= -2) {
        int fi = -2 - zoomie_target;
        if (fi >= Cfg::MAX_FIREFLIES || !ch.fireflies[fi].active) done = true;
    }

    if (done) {
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        speed = Cfg::ROLE_PARAMS[role].speed;
        zoomie_target = -1;
        zoomie_ticks = 0;
        interaction_cooldown = Cfg::INTERACTION_COOLDOWN_TICKS;
        idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                               Cfg::IDLE_REST_MAX_TICKS);
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        _pick_idle_microstate(ch);
        return;
    }

    if (zoomie_target <= -2) {
        // Firefly chase — and the catch: a triumphant hop as it slips free
        Firefly& f = ch.fireflies[-2 - zoomie_target];
        float dx = f.x - x, dy = f.y - y;
        if (dx * dx + dy * dy < 0.8f) {
            f.active = false;          // it flits away into the dark
            anim_type = LG_ANIM_NOTICE;
            anim_remaining_ticks = 12;
            zoomie_ticks = 1;          // wind down next tick
            return;
        }
        _step_toward_cell(static_cast<int>(f.x), static_cast<int>(f.y), ch);
    } else if (zoomie_target < 0) {
        // Runner: pick random waypoints and sprint to them
        int cx = cell_x(), cy = cell_y();
        if (!has_target || (cx == target_x && cy == target_y)) {
            // Pick a new random waypoint within the grid
            target_x = g_rng.rand_int(1, Cfg::GRID_WIDTH - 2);
            target_y = g_rng.rand_int(1, Cfg::GRID_HEIGHT - 2);
            has_target = true;
        }
        _step_toward_cell(target_x, target_y, ch);
    } else {
        // Chaser: trail a couple cells behind target's facing direction
        auto& t = ch.conkers[zoomie_target];
        int tx = t.cell_x() - static_cast<int>(t.facing_dx * 2.0f);
        int ty = t.cell_y() - static_cast<int>(t.facing_dy * 2.0f);
        tx = (tx < 0) ? 0 : (tx >= Cfg::GRID_WIDTH  ? Cfg::GRID_WIDTH  - 1 : tx);
        ty = (ty < 0) ? 0 : (ty >= Cfg::GRID_HEIGHT ? Cfg::GRID_HEIGHT - 1 : ty);
        _step_toward_cell(tx, ty, ch);
    }
}

// ================================================================
//  Idle/rest
// ================================================================

void Conker::_do_mourning(Chamber& ch) {
    // zoomie_ticks is repurposed as the vigil timer (states are exclusive)
    zoomie_ticks--;
    if (zoomie_ticks <= 0) {
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                               Cfg::IDLE_REST_MAX_TICKS);
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        _pick_idle_microstate(ch);
        return;
    }

    // Walk to the husk
    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) > 1) {
        _step_toward_cell(target_x, target_y, ch);
        return;
    }

    // At the husk — stand vigil, occasionally bowing toward it
    if (anim_type == LG_ANIM_NONE && g_rng.rand_float() < 0.03f) {
        anim_type = LG_ANIM_GROOMING;
        anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
        float bdx = (target_x + 0.5f) - x, bdy = (target_y + 0.5f) - y;
        if (fabsf(bdx) >= fabsf(bdy)) {
            anim_lean_dx = (bdx >= 0) ? 1 : -1; anim_lean_dy = 0;
        } else {
            anim_lean_dx = 0; anim_lean_dy = (bdy >= 0) ? 1 : -1;
        }
    }
}

void Conker::_tick_idle(Chamber& ch) {
    idle_ticks_remaining--;
    idle_micro_ticks--;

    // Timer expired → exit idle
    if (idle_ticks_remaining <= 0) {
        speed = Cfg::ROLE_PARAMS[role].speed;
        has_target_cell = false;
        _pick_task(ch);
        return;
    }

    // A nearby glow is irresistible to a restless night idler
    if (g_tod.night_factor >= Cfg::FIREFLY_NIGHT_FACTOR_MIN
            && !sleeping && stack_on < 0 && anim_type == LG_ANIM_NONE
            && g_rng.rand_float() < Cfg::FIREFLY_CHASE_CHANCE) {
        int fi = ch.nearest_firefly(cell_x(), cell_y(), Cfg::FIREFLY_CHASE_RADIUS);
        if (fi >= 0) {
            state = STATE_ZOOMIES;
            zoomie_target = -2 - fi;  // <= -2 encodes a firefly chase
            zoomie_ticks = Cfg::FIREFLY_CHASE_MAX_TICKS;
            has_target = false;
            has_target_cell = false;
            idle_ticks_remaining = 0;
            return;
        }
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

void Conker::_pick_idle_microstate(Chamber& ch) {
    has_target_cell = false;

    float r = g_rng.rand_float();
    if (r < Cfg::IDLE_HOLD_WEIGHT) {
        idle_microstate = 0;  // hold
        speed = Cfg::ROLE_PARAMS[role].speed;
    } else if (r < Cfg::IDLE_HOLD_WEIGHT + Cfg::IDLE_DRIFT_WEIGHT) {
        idle_microstate = 1;  // random drift
        speed = Cfg::IDLE_DRIFT_SPEED;
    } else if (r < Cfg::IDLE_HOLD_WEIGHT + Cfg::IDLE_DRIFT_WEIGHT + Cfg::IDLE_HUDDLE_WEIGHT) {
        idle_microstate = 3;  // huddle: drift toward nearest idler or queen
        speed = Cfg::IDLE_DRIFT_SPEED;
    } else {
        idle_microstate = 2;  // reface
        speed = Cfg::ROLE_PARAMS[role].speed;
        int d = g_rng.rand_int(0, 3);
        const float fdx[] = {1.0f, -1.0f, 0.0f, 0.0f};
        const float fdy[] = {0.0f, 0.0f, 1.0f, -1.0f};
        facing_dx = fdx[d];
        facing_dy = fdy[d];
    }
    // work_tempo biases micro duration: high tempo = shorter micro-states
    float tempo_scale = 1.3f - 0.6f * personality[PERS_WORK_TEMPO];  // 0.7 to 1.3
    int micro_dur = g_rng.rand_int(Cfg::IDLE_MICROSTATE_MIN_TICKS,
                                    Cfg::IDLE_MICROSTATE_MAX_TICKS);
    idle_micro_ticks = static_cast<int16_t>(micro_dur * tempo_scale);
}

float Conker::_colony_idle_budget(Chamber& ch) {
    if (ch.colony->population < Cfg::COLONY_MIN_ACTIVE_FOR_IDLE) return 0.0f;
    if (ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE) return 0.0f;
    // Blend idle budget by night_factor: day=0.70, night=0.95
    float nf = g_tod.night_factor;
    return Cfg::IDLE_BUDGET_DAY + nf * (Cfg::IDLE_BUDGET_NIGHT - Cfg::IDLE_BUDGET_DAY);
}

// ================================================================
//  Task validity
// ================================================================

bool Conker::_target_still_valid(Chamber& ch) {
    if (state == STATE_IDLE || state == STATE_TO_FOOD
            || state == STATE_TO_HOME || state == STATE_CANNIBALIZE
            || state == STATE_ZOOMIES || state == STATE_EATING
            || state == STATE_MOURNING)
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

bool Conker::_sample_markers(Chamber& ch, bool use_food, int8_t& out_dx, int8_t& out_dy) {
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

    // Trail defection: chance to ignore the gradient and wander instead.
    // Base chance applies to everyone — strays shortcut bends in the trail
    // (their straighter return lays a stronger line) and find new piles.
    float explore_chance = Cfg::TRAIL_DEFECT_BASE
                         + personality[PERS_EXPLORATION] * Cfg::TRAIL_DEFECT_PERS;
    if (explore_chance > 0 && g_rng.rand_float() < explore_chance) return false;

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

void Conker::_step_toward_cell(int tx, int ty, Chamber& ch) {
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

void Conker::_persistent_forward_step(Chamber& ch) {
    // Quantize facing to cardinal
    int fx, fy;
    if (fabsf(facing_dx) >= fabsf(facing_dy)) {
        fx = (facing_dx > 0) ? 1 : -1; fy = 0;
    } else {
        fx = 0; fy = (facing_dy > 0) ? 1 : -1;
    }

    // Personality gait while foraging outbound: sticky conkers march
    // straight (stronger return trails), explorers weave wide arcs.
    float fwd = 0.70f, rev = 0.06f;
    if (state == STATE_TO_FOOD) {
        float stick = personality[PERS_ROUTE_STICKINESS];
        float expl  = personality[PERS_EXPLORATION];
        fwd = 0.55f + 0.30f * stick - 0.10f * expl;   // 0.45 to 0.85
        rev = 0.03f + 0.06f * (1.0f - stick);         // 0.03 to 0.09
    }
    float turn = (1.0f - fwd - rev) * 0.5f;

    float r = g_rng.rand_float();
    int dx, dy;
    if (r < fwd)                  { dx = fx;  dy = fy; }
    else if (r < fwd + turn)      { dx = -fy; dy = fx; }   // left
    else if (r < fwd + 2 * turn)  { dx = fy;  dy = -fx; }  // right
    else                          { dx = -fx; dy = -fy; }  // reverse

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

bool Conker::_flair_allowed(Chamber& ch) {
    // Stressed colony = all business
    return ch.colony->food_pressure() <= Cfg::FLAIR_MAX_PRESSURE;
}

int Conker::_max_foragers(Chamber& ch) {
    float pressure = ch.colony->food_pressure();
    float frac = Cfg::BASE_FORAGER_FRACTION
        + (1.0f - Cfg::BASE_FORAGER_FRACTION)
          * (pressure / Cfg::FAMINE_SLOWDOWN_PRESSURE);
    // Waggle recruitment: a fresh delivery advertises a productive source.
    // The boost rides food_delivery_signal, so sustained deliveries hold the
    // swarm open and it dissolves as the signal decays. Recruits navigate by
    // the laid trail gradient — nothing is steered directly.
    frac += Cfg::RECRUIT_SIGNAL_FRACTION * (ch.food_delivery_signal / 200.0f);
    if (frac > 1.0f) frac = 1.0f;
    int mf = static_cast<int>(ch.colony->population * frac + 0.5f);
    return (mf < 1) ? 1 : mf;
}

void Conker::_explore_with_flair(Chamber& ch) {
    // Only reached when truly blind: no food sensed, no trail gradient.
    // Occasionally stop and cast about before wandering on.
    if (flair_casts_used < Cfg::CAST_MAX_PER_TRIP && _flair_allowed(ch)
            && g_rng.rand_float() < Cfg::CAST_CHANCE) {
        flair_casts_used++;
        flair_kind = 1;
        flair_ticks = Cfg::CAST_DURATION_TICKS;
        return;
    }
    _explore_or_wander(ch);
}

void Conker::_explore_or_wander(Chamber& ch) {
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

bool Conker::_detects_food_trail(Chamber& ch) {
    return ch.food_delivery_signal > 0;
}

int Conker::_nearest_hungry_larva(Chamber& ch) {
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

int Conker::_least_invested_larva(Chamber& ch) {
    int best = -1;
    float best_fed = 1000000.0f;
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_LARVA || !b.alive()) continue;
        if (b.food_invested < best_fed) { best = i; best_fed = b.food_invested; }
    }
    return best;
}

bool Conker::_food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y) {
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

bool Conker::_nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
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
