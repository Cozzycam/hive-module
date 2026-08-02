/* Worker conker -- full behavior port with smooth sub-cell movement. */
#include "conker.h"
#include "chamber.h"
#include "bonds.h"
#include "rng.h"
#include "time_of_day.h"
#include "weather.h"
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
    needs[NEED_BOREDOM] = 0.0f;
    // Seed tiredness from time of day, jittered per conker so a night reboot
    // doesn't bunch everyone at identical max-tired (they wake staggered).
    needs[NEED_REST]    = g_tod.night_factor * (0.8f + 0.2f * g_rng.rand_float());
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
    // (all 8 dims now randomized — bravery/playfulness/appetite are live, no reserve)

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

    // Incubation/princess mode (Gateway tamagotchi): a lone raised conker is
    // queen-like — she never dies, only goes DORMANT when neglected. While
    // dormant she is suspended (no behaviour), losing only maturation progress
    // + keeper-bond until you resume care (a keeper feed drops her hunger and
    // rouses her). Inert on hardware / normal colonies (incubation_mode = false).
    if (ch.incubation_mode && dormant) {
        if (keeper_bond > 0.0f) {
            keeper_bond -= Cfg::KEEPER_BOND_DECAY_DORMANT * dt;
            if (keeper_bond < 0.0f) keeper_bond = 0.0f;
        }
        uint32_t erode = static_cast<uint32_t>(Cfg::MATURATION_DECAY_MS_PER_SEC * dt);
        lived_ms = (lived_ms > erode) ? lived_ms - erode : 0;
        // A keeper dropping food nearby rouses her — she stirs, then toddles over
        // to eat it (she can't forage while suspended, so food presence is the wake).
        // A thrown ball rouses her the same way: it's the same thing, someone came back.
        if (ch.food_pile_count > 0 || ch.has_ball()) { dormant = false; if (hunger > Cfg::DORMANT_WAKE_HUNGER) hunger = Cfg::DORMANT_WAKE_HUNGER; }
        return;
    }

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
            target_x = ch.clamp_room_x(target_x);   // whole grid on a colony
            target_y = ch.clamp_room_y(target_y);
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
            x = ch.clamp_room_x(x);
            y = ch.clamp_room_y(y);
        }
        return;
    }

    // Accumulate lived time (only advances while sim is running)
    lived_ms += static_cast<uint32_t>(dt * 1000.0f);

    // Princess (incubation mode): matures to "queen-ready" then holds, and
    // never ages out — she's queen-like, awaiting coronation.
    if (ch.incubation_mode && lived_ms > Cfg::PRINCESS_READY_MS)
        lived_ms = Cfg::PRINCESS_READY_MS;

    if (lived_ms >= lifespan_ms && !ch.incubation_mode) {
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
        // Princess (incubation mode): neglect suspends her — she does not starve
        // to death. Park hunger just below the threshold and go dormant.
        if (ch.incubation_mode) {
            dormant = true;
            hunger = Cfg::DORMANT_HUNGER_PARK;
            return;
        }
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

    // Needs / mood (boredom etc.) — evolve drives + derive the display mood
    _update_needs(ch, dt);

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
    // tick). Firefly chases use a gentler night-dash; parades a gentle trot.
    if (state == STATE_ZOOMIES) {
        speed *= (zoomie_target <= -2) ? Cfg::FIREFLY_CHASE_SPEED_MULT
               : (zoomie_style == 1)   ? Cfg::PARADE_SPEED_MULT
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

    // Return-home timer — crisis overrides animation. The posted gardener is
    // exempt: being away IS the job; her needs bring her home instead (v189).
    if (ticks_away >= Cfg::RETURN_HOME_TICKS && state != STATE_TO_HOME
            && !(ch.is_garden && ch.posted_gardener == id)
            && !ch.incubation_mode) {   // a lone princess has no home to be marched back to
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
                if (_should_wake(ch)) {
                    bool lonely_wake = _wants_company_wake(ch);
                    sleeping = false;
                    anim_type = LG_ANIM_NONE;
                    stack_on = -1;  // unstack to go hustle
                    if (lonely_wake) {
                        seeking_company = true;
                        seek_ticks = Cfg::SOCIAL_SEEK_TIMEOUT_TICKS;
                    }
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
        if (_should_wake(ch)) {
            bool lonely_wake = _wants_company_wake(ch);
            sleeping = false;
            anim_type = LG_ANIM_NONE;
            if (lonely_wake) {
                seeking_company = true;
                seek_ticks = Cfg::SOCIAL_SEEK_TIMEOUT_TICKS;
            }
        } else {
            anim_type = LG_ANIM_SNOOZE;
            // Sleep isn't stillness: now and then a sleeper turns over or
            // resettles half a step — a pile of creatures, not statues.
            if (g_rng.rand_float() < Cfg::SLEEP_TWITCH_CHANCE) {
                if (stack_on < 0 && g_rng.rand_int(0, 2) == 0) {
                    // Resettle: a tiny shuffle in place
                    float nx = x + (g_rng.rand_float() - 0.5f) * 0.4f;
                    if (nx >= 1.0f && nx < Cfg::GRID_WIDTH - 1.0f) x = nx;
                } else {
                    // Turn over
                    facing_dx = (facing_dx >= 0.0f) ? -1.0f : 1.0f;
                    last_dx = facing_dx;
                }
            }
            return;
        }
    }

    // Past the sleeping branches above, this conker is awake. A lingering snooze
    // sprite here is stale (e.g. blankslate clears `sleeping` but not the anim, or
    // any wake path that missed it) — drop it so she doesn't sleep-walk. SNOOZE is
    // a persistent-state anim (no anim_remaining_ticks), so nothing else clears it.
    if (anim_type == LG_ANIM_SNOOZE) anim_type = LG_ANIM_NONE;

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

    // v163: an awake, lonely, sociable conker that's alone ambles over to company —
    // the daytime counterpart of the night huddle-wake. Only when idle, so it never
    // interrupts foraging/tending.
    if (!seeking_company && state == STATE_IDLE && anim_type == LG_ANIM_NONE
            && _wants_company_awake(ch)) {
        seeking_company = true;
        seek_ticks = Cfg::SOCIAL_SEEK_TIMEOUT_TICKS;
    }
    // v150: a conker that woke lonely owns its tick — groggily padding over to a
    // friend to huddle, then resettling — until it's nestled in or dawn breaks.
    if (seeking_company) {
        _tick_seek_company(ch);
        return;
    }

    // Movement phase -- every tick
    _advance_toward_target(ch);

    // Decision phase -- only when arrived (no pending target)
    if (state == STATE_IDLE) {
        // A thrown ball cuts an idle rest short. Without this she only notices it
        // whenever her next task re-pick happens to come round (a rest runs 5-30s,
        // and idle_cooldown can push it further) — so the keeper throws, and she
        // sits there. The ball has to interrupt, the way being tapped does.
        if (ch.incubation_mode && ch.has_ball()) {
            idle_ticks_remaining = 0;
            idle_cooldown = 0;
            _pick_task(ch);
        } else if (idle_ticks_remaining > 0) {
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
            case STATE_FARMING:     _do_farming(ch);     break;
            case STATE_CRAFTING:    _do_crafting(ch);    break;
            case STATE_TO_GARDEN:   _do_to_garden(ch);   break;
            case STATE_PLAYING:     _do_play_ball(ch);   break;
            default:                _do_idle(ch);        break;
        }
    }
}

// ================================================================
//  Movement engine
// ================================================================

bool Conker::_set_target_cell(int cx, int cy, Chamber& ch) {
    if (!ch.in_bounds(cx, cy)) return false;
    // Her room's walls. Every step routes through here, so this is the one place
    // that has to hold — and reusing the existing "blocked cell" return means she
    // slides along a wall exactly as she does around the queen's body, rather than
    // stopping dead. No-op on a normal colony (room_* spans the whole grid).
    if (cx < ch.room_x0() || cx > ch.room_x1() || cy < ch.room_y0() || cy > ch.room_y1())
        return false;
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
    if (state == STATE_TO_GARDEN)
        Serial.printf("[garden] %s re-tasked off the trip mid-walk\r\n", name);
    speed = base_speed();
    idle_ticks_remaining = 0;
    zoomie_target = -1;
    zoomie_ticks = 0;
    zoomie_style = 0;
    sowing = false;
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

    // Incubation/princess (Gateway tamagotchi): a lone raised conker with no
    // queen or food store. When peckish she eats directly from the food piles
    // the keeper drops (STATE_EATING routes to the nearest pile in _do_eating);
    // otherwise she potters/plays via the normal idle arbiter below. She skips
    // the no-queen "go home" sweep (there is no home to go to).
    if (ch.incubation_mode) {
        if (hunger > Cfg::PRINCESS_EAT_FLOOR && ch.food_pile_count > 0) {
            state = STATE_EATING;
            has_target = false;
            has_target_cell = false;
            return;
        }
        // A ball in play beats pottering — it's the keeper asking to play, and
        // it's the one interaction that isn't tapping her.
        if (ch.has_ball()) {
            state = STATE_PLAYING;
            has_target = false;
            has_target_cell = false;
            return;
        }
    }

    // The garden post: a green thumb on the garden module holds (or takes)
    // the post instead of being swept home by the no-queen rule below. Their
    // own needs still outrank the job — a posted gardener too hungry to work
    // (no food store here) or lonely with no company around steps down and
    // heads home; the vacancy is advertised via pop sync and the queen sends
    // a replacement. Priority: carrying food > fulfilling need > gardening > idling.
    // (No famine gate here: the satellite's local food_store is always ~0 so
    // its food_pressure() reads as permanent famine — v190 field bug that
    // turned every arriving gardener straight home. The queen famine-gates
    // the summons; local hunger below still brings the gardener home.)
    bool posted_here = false;
    if (ch.is_garden && !ch.has_queen
            && green_thumb() >= Cfg::GREEN_THUMB_MIN) {
        bool hungry = hunger > Cfg::GARDENER_HUNGER_HOME;
        if (hungry || _wants_company_awake(ch)) {
            if (ch.posted_gardener == id) {
                ch.garden_post_release(id);
                Serial.printf("[garden] %s steps down from the post (%s)\r\n",
                              name, hungry ? "hungry" : "lonely");
            }
        } else if (ch.garden_post_claim(id)) {
            posted_here = true;
        }
    }

    if (!posted_here && !ch.has_queen && !ch.incubation_mode) {
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
        // v161: hunger drives eating, scaled by APPETITE — gluttons eat sooner,
        // ascetics hold out longer. Driven ramp; the roll is only timing jitter.
        float eat_floor = 35.0f - 25.0f * personality[PERS_APPETITE];  // ~10 (glutton)..35 (ascetic); avg≈22 ~ old
        float eat_chance = (hunger - eat_floor) / 40.0f;
        if (eat_chance > 1.0f) eat_chance = 1.0f;

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

    // Sleep check (tiredness-driven) — settle for the night once reasonably
    // tired, or power-nap any time if exhausted. Famine keeps everyone hustling.
    {
        bool nightish = (g_tod.phase == PHASE_DUSK || g_tod.phase == PHASE_NIGHT
                      || g_tod.phase == PHASE_DAWN);
        float tired = needs[NEED_REST];
        // Chronotype: dozy ones cross the any-time (daytime-nap) line sooner.
        // Night owls skip the easy "it's dark, go to bed" trigger entirely —
        // they only turn in via their (night-lifted) personal threshold.
        bool wants_sleep = (tired >= _nap_threshold())
                        || (tired >= Cfg::TIRED_SLEEP_NIGHT && nightish
                            && !is_night_owl());
        if (!was_sleeping && !sleeping && wants_sleep
                && pressure <= Cfg::FAMINE_SLOWDOWN_PRESSURE) {
            stack_on = was_stacked;
            sleeping = true;
            // A sleep that begins in the day is a nap — a short top-up that wakes
            // once it's restored TIRED_NAP_RESTORE, then back to work. One that
            // begins at dusk/night is a night sleep that holds through to dawn.
            daytime_nap = (g_tod.phase == PHASE_DAY);
            if (daytime_nap)
                nap_wake_target = tired - Cfg::TIRED_NAP_RESTORE;
            anim_type = LG_ANIM_SNOOZE;
            state = STATE_IDLE;
            has_target = false;
            has_target_cell = false;
            idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                                   Cfg::IDLE_REST_MAX_TICKS);
            return;
        }
    }

    // Gardening: the posted gardener works the plots by day — a real job,
    // not an idle pastime. It yields to hauling food and to their own needs
    // (both handled above) and pre-empts idling. With nothing to sow they
    // stay at ease on the post rather than being drafted into other work.
    if (posted_here) {
        if (g_tod.phase == PHASE_DAY) {
            int plot = ch.free_plot();
            if (plot >= 0) {
                state = STATE_FARMING;
                zoomie_target = plot;              // repurposed: plot index
                zoomie_ticks = Cfg::FARM_MAX_TICKS;
                has_target = false;
                has_target_cell = false;
                return;
            }
        }
        stack_on = was_stacked;
        sleeping = was_sleeping;
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                               Cfg::IDLE_REST_MAX_TICKS);
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        if (was_stacked < 0 && !sleeping) _pick_idle_microstate(ch);
        return;
    }

    // Idle budget — gates all non-critical tasks.
    // Returns 0 during famine or founding, so crisis paths still fire.
    // work_tempo biases: high tempo workers are less likely to idle
    float budget = _colony_idle_budget(ch);
    float tempo_bias = 1.3f - 0.6f * personality[PERS_WORK_TEMPO];  // 0.7 to 1.3
    if (budget > 0 && g_rng.rand_float() < budget * tempo_bias) {
        // Stacked idlers don't lounge forever: restless personalities hop
        // down and potter about instead of re-perching every idle cycle
        // (feedback: "90% of the time they prefer to stay stacked")
        if (was_stacked >= 0 && !was_sleeping) {
            float restless = 0.5f * personality[PERS_WORK_TEMPO]
                           + 0.5f * personality[PERS_EXPLORATION];
            if (g_rng.rand_float() < 0.30f + 0.45f * restless) {
                was_stacked = -1;  // dismount and idle on the ground
                stack_cooldown_ms = millis()
                    + static_cast<uint32_t>(g_rng.rand_int(20000, 45000));
            }
        }
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

    // Night stand-down: no foraging expeditions in the dark unless the
    // colony is genuinely hungry. A zero-pressure midnight forager pacing
    // between modules hunting piles that don't exist reads as broken, not
    // busy (night watch, 2026-07-03). Famine overrides — survival first.
    bool forage_hours = (g_tod.phase != PHASE_NIGHT)
                     || pressure >= Cfg::NIGHT_FORAGE_MIN_PRESSURE;

    // Incubation/princess: no colony to forage for — she never becomes a
    // forager (that loop, TO_FOOD→TO_HOME, would trap her out of eating/idling).
    // She eats from keeper piles when hungry (handled above) and potters/plays
    // otherwise (falls through to idle below).
    if (forage_hours && col->gatherer_count < max_foragers && !ch.incubation_mode) {
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
            int linger = static_cast<int>(personality[PERS_APPETITE]
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
        } else if (!_forage_follow_friend(ch)) {
            _explore_with_flair(ch);
        }
    } else if (!_forage_follow_friend(ch)) {
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

// The ball the keeper threw (incubation only). She runs it down, pounces, knocks
// it on, and chases again until it rolls to a stop. This exists because tapping
// was the ONLY way to reach her, so bonding degenerated into spam-tapping the
// loneliness bar — a chase is company you give her rather than a button you hold.
void Conker::_do_play_ball(Chamber& ch) {
    if (!ch.has_ball()) {                 // rolled to a stop while she closed in
        state = STATE_IDLE;
        has_target = false; has_target_cell = false;
        return;
    }
    int8_t bx = ch.ball_x, by = ch.ball_y;
    if (abs(bx - cell_x()) + abs(by - cell_y()) <= 1) {
        needs[NEED_BOREDOM] -= Cfg::BALL_BOREDOM_RELIEF;
        if (needs[NEED_BOREDOM] < 0.0f) needs[NEED_BOREDOM] = 0.0f;
        needs[NEED_SOCIAL]  -= Cfg::BALL_SOCIAL_RELIEF;
        if (needs[NEED_SOCIAL] < 0.0f) needs[NEED_SOCIAL] = 0.0f;
        keeper_bond += Cfg::KEEPER_BOND_PER_PLAY;
        if (keeper_bond > 1.0f) keeper_bond = 1.0f;
        afterglow_ticks = Cfg::AFTERGLOW_TICKS;    // she's pleased with herself
        anim_type = LG_ANIM_NOTICE;                // the startle hop reads as a pounce
        anim_remaining_ticks = Cfg::BALL_PLAY_DURATION_TICKS;
        float qdx = bx - x, qdy = by - y;          // lean into it, as eating does
        if (fabsf(qdx) >= fabsf(qdy)) { anim_lean_dx = (qdx > 0) ? 1 : -1; anim_lean_dy = 0; }
        else                          { anim_lean_dx = 0; anim_lean_dy = (qdy > 0) ? 1 : -1; }
        ch.bat_ball();
        has_target = false; has_target_cell = false;
        return;   // stay in STATE_PLAYING — next tick chases wherever it landed
    }
    _step_toward_cell(bx, by, ch);
}

void Conker::_do_eating(Chamber& ch) {
    // Incubation/princess: no queen or store — she walks to the nearest food
    // pile the keeper dropped and eats from it directly. Eating the food you
    // gave her is what deepens the keeper-bond (not raw taps).
    if (ch.incubation_mode) {
        if (ch.food_pile_count == 0) {
            state = STATE_IDLE; has_target = false; has_target_cell = false;
            return;
        }
        int best = -1, bestd = 1 << 30;
        for (int i = 0; i < ch.food_pile_count; i++) {
            int dx = ch.food_piles[i].x - cell_x(), dy = ch.food_piles[i].y - cell_y();
            int d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; best = i; }
        }
        int8_t fx = ch.food_piles[best].x, fy = ch.food_piles[best].y;
        if (abs(fx - cell_x()) + abs(fy - cell_y()) <= 1) {
            ch.take_food(fx, fy, Cfg::WORKER_MEAL_COST * 2.0f);   // a bite from the pile
            hunger = 0.0f;
            speed = base_speed();
            keeper_bond += Cfg::KEEPER_BOND_PER_FEED;             // fed by hand → closer to you
            if (keeper_bond > 1.0f) keeper_bond = 1.0f;
            anim_type = LG_ANIM_GROOMING;
            anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
            float qdx = fx - x, qdy = fy - y;
            if (fabsf(qdx) >= fabsf(qdy)) { anim_lean_dx = (qdx > 0) ? 1 : -1; anim_lean_dy = 0; }
            else { anim_lean_dx = 0; anim_lean_dy = (qdy > 0) ? 1 : -1; }
            has_target = false; has_target_cell = false;
            _pick_task(ch);
            return;
        }
        _step_toward_cell(fx, fy, ch);
        return;
    }
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
            speed = base_speed();  // clear any starvation penalty
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
        if (idle_microstate == 5) {
            // Pirouette: steady twirl in place, spin direction by identity
            if ((idle_micro_ticks & 1) == 0) {
                float fdx = facing_dx, fdy = facing_dy;
                if (id & 1) { facing_dx = -fdy; facing_dy = fdx;  }
                else        { facing_dx = fdy;  facing_dy = -fdx; }
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
                // Gravitate to friends — best friends most of all.
                if (_is_best_friend(ch, other.id))  d -= 2 * Cfg::FRIEND_HUDDLE_PULL;
                else if (_is_friend(ch, other.id))  d -= Cfg::FRIEND_HUDDLE_PULL;
                // A friend in their twilight draws everyone who loves them
                // close — the final days are spent in company
                if (other.is_twilight() && _is_friend(ch, other.id))
                    d -= 3 * Cfg::FRIEND_HUDDLE_PULL;
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

    // End conditions: timer expired, chaser's target invalid or stopped playing
    bool done = (zoomie_ticks <= 0);
    if (!done && zoomie_target >= 0
        && (zoomie_target >= ch.conker_count
            || !ch.conkers[zoomie_target].alive
            || ch.conkers[zoomie_target].state != STATE_ZOOMIES)) {
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
        speed = base_speed();
        zoomie_target = -1;
        zoomie_ticks = 0;
        zoomie_style = 0;
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
            // A caught firefly is a find too — the night critter. Lets
            // discoveries (and their notifications) happen after dark.
            needs[NEED_BOREDOM] -= Cfg::DISCOVERY_BOREDOM_RELIEF;
            if (needs[NEED_BOREDOM] < 0.0f) needs[NEED_BOREDOM] = 0.0f;
            afterglow_ticks = Cfg::AFTERGLOW_TICKS;   // a find is a delight
            // A firefly is a night whimsy, not a bug you hunt — it still counts
            // as a discovery (sparkle + after-dark notification, the whole point
            // of the chase) but NOT toward the Bug Hunter title. Fireflies spawn
            // ~17x faster than real critters, so counting them here pumped the
            // catch tally and flipped the crown constantly. Only beetle/butterfly/
            // worm catches (chamber.cpp) earn the Catcher trait now.
            Event fev;
            fev.type = EVT_DISCOVERY;
            fev.tick = ch.tick_num;
            fev.discovery = { static_cast<uint8_t>(this - ch.conkers), CRITTER_FIREFLY };
            ch.emit(fev);
            return;
        }
        _step_toward_cell(static_cast<int>(f.x), static_cast<int>(f.y), ch);
    } else if (zoomie_target < 0) {
        // Runner: pick random waypoints and sprint to them
        int cx = cell_x(), cy = cell_y();
        if (!has_target || (cx == target_x && cy == target_y)) {
            // Pick a new random waypoint within the grid
            // Inset by one ring, as before — for a normal colony room_* spans the
            // whole grid so this is still exactly rand_int(1, GRID_WIDTH-2).
            target_x = g_rng.rand_int(ch.room_x0() + 1, ch.room_x1() - 1);
            target_y = g_rng.rand_int(ch.room_y0() + 1, ch.room_y1() - 1);
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
        // Grief makes makers: sometimes the vigil ends not in walking away
        // but in carving something that stays — a memorial with their
        // friend's name on it
        if (muse() >= Cfg::MUSE_MIN && mourning_for[0]
                && g_rng.rand_float() < Cfg::CRAFT_GRIEF_CHANCE
                && _start_crafting(ch, ART_MEMORIAL, CTX_GRIEF,
                                   target_x, target_y)) {
            return;   // mourning_for carries the honoree into the stone
        }
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

// The moment of a work's making, read from the sky (grief is set by the
// mourning path instead — it outranks weather).
static uint8_t _craft_context_now() {
    if (g_weather.valid) {
        if (g_weather.condition == WX_THUNDERSTORM || g_weather.wind >= WIND_HIGH)
            return CTX_STORM;
        if (g_weather.temp >= TEMP_HOT) return CTX_HEATWAVE;
        if (g_weather.condition >= WX_DRIZZLE && g_weather.condition <= WX_HEAVY_RAIN)
            return CTX_RAIN;
    }
    if (g_tod.phase == PHASE_NIGHT) return CTX_NIGHT;
    return CTX_PLENTY;
}

// A finished piece rests the muse — keener makers rest less, so the most
// inclined manage roughly a work a day and borderline ones every couple of
// days. Squared falloff: the truly keen pull well ahead of the merely able.
void Conker::_rest_muse() {
    if (g_tod.unix_time < 1000000) return;   // clock not synced yet — let it slide
    float t = (muse() - Cfg::MUSE_MIN) / (1.0f - Cfg::MUSE_MIN);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float hours = Cfg::MUSE_REST_MIN_HOURS
                + (1.0f - t) * (1.0f - t)
                  * (Cfg::MUSE_REST_MAX_HOURS - Cfg::MUSE_REST_MIN_HOURS);
    hours *= 0.85f + 0.3f * g_rng.rand_float();   // never on a schedule
    muse_rest_unix = g_tod.unix_time + (uint32_t)(hours * 3600.0f);
}

// Find a clear spot near (near_x, near_y) and set out to make something.
bool Conker::_start_crafting(Chamber& ch, uint8_t kind, uint8_t context,
                             int near_x, int near_y) {
    for (int attempt = 0; attempt < 12; attempt++) {
        int sx = near_x + g_rng.rand_int(-3, 3);
        int sy = near_y + g_rng.rand_int(-3, 3);
        if (!ch.artwork_spot_free(sx, sy)) continue;
        state = STATE_CRAFTING;
        craft_kind = kind;
        craft_context = context;
        craft_ticks = Cfg::CRAFT_DURATION_TICKS;
        target_x = (int8_t)sx;
        target_y = (int8_t)sy;
        has_target = true;
        has_target_cell = false;
        zoomie_target = -1;
        zoomie_ticks = Cfg::CRAFT_MAX_TICKS;   // repurposed: trip failsafe
        idle_ticks_remaining = 0;
        return true;
    }
    return false;
}

// At work on a piece: walk to the chosen spot, lean in and make (visible
// craftsmanship over real minutes), then the object joins the world in the
// maker's own colours.
void Conker::_do_crafting(Chamber& ch) {
    zoomie_ticks--;
    if (zoomie_ticks <= 0) {   // trip failsafe — abandon gracefully
        state = STATE_IDLE;
        anim_type = LG_ANIM_NONE;
        has_target = false;
        has_target_cell = false;
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        return;
    }

    int cx = cell_x(), cy = cell_y();
    if (abs(target_x - cx) + abs(target_y - cy) > 1) {
        _step_toward_cell(target_x, target_y, ch);
        return;
    }

    // At the spot — lean in and work
    if (anim_type != LG_ANIM_GROOMING) {
        anim_type = LG_ANIM_GROOMING;   // bent over the piece
        anim_remaining_ticks = 0;       // persistent pose, not a timed anim
        float bdx = (target_x + 0.5f) - x, bdy = (target_y + 0.5f) - y;
        if (fabsf(bdx) >= fabsf(bdy)) {
            anim_lean_dx = (bdx >= 0) ? 1 : -1; anim_lean_dy = 0;
        } else {
            anim_lean_dx = 0; anim_lean_dy = (bdy >= 0) ? 1 : -1;
        }
    }
    if (craft_ticks > 0) { craft_ticks--; return; }

    // Finished. Accessories are gifts — worn, not placed: find the friend,
    // put the petal hat on them, and let the whole colony know.
    if (craft_kind >= ART_HAT && craft_kind <= ART_BAND) {
        for (int i = 0; i < ch.conker_count; i++) {
            Conker& o = ch.conkers[i];
            if (o.id != craft_for || !o.alive) continue;
            if (o.accessory == 0) {
                o.accessory = (uint8_t)(craft_kind - ART_HAT + 1);
                o.accessory_tint = tint_seed;   // the hat wears the MAKER's colour
                o.accessory_from = id;
                o.accessory_memorial = false;
                if (ch.bonds) {   // a gift deepens the bond, both ways
                    ch.bonds->increment(id, o.id, Cfg::GIFT_BOND_BOOST);
                    ch.bonds->increment(o.id, id, Cfg::GIFT_BOND_BOOST);
                }
                Event ev = {};
                ev.type = EVT_CRAFTED;
                ev.tick = ch.tick_num;
                ev.crafted.kind = craft_kind;
                ev.crafted.context = craft_context;
                ev.crafted.maker_id = id;
                strlcpy(ev.crafted.who, name, sizeof(ev.crafted.who));
                strlcpy(ev.crafted.honoree, o.name, sizeof(ev.crafted.honoree));
                ch.emit(ev);
            }
            break;
        }
        craft_for = 0;
        anim_type = LG_ANIM_NONE;
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        afterglow_ticks = Cfg::AFTERGLOW_TICKS;
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        _rest_muse();
        return;
    }

    // Placed works — the piece joins the world
    Artwork piece;
    piece.kind = craft_kind;
    piece.x = target_x;
    piece.y = target_y;
    piece.maker_id = id;
    strlcpy(piece.maker_name, name, sizeof(piece.maker_name));
    piece.maker_tint = tint_seed;
    piece.created_unix = g_tod.unix_time;
    piece.context = craft_context;
    piece.motif = (uint8_t)g_rng.rand_int(0, 255);
    if (craft_kind == ART_MEMORIAL)
        strlcpy(piece.honoree, mourning_for, sizeof(piece.honoree));

    Artwork weathered;
    ch.place_artwork(piece, &weathered);
    if (weathered.active) {
        Event wv = {};
        wv.type = EVT_ART_WEATHERED;
        wv.tick = ch.tick_num;
        wv.crafted.kind = weathered.kind;
        strlcpy(wv.crafted.who, weathered.maker_name, sizeof(wv.crafted.who));
        ch.emit(wv);
    }
    Event ev = {};
    ev.type = EVT_CRAFTED;
    ev.tick = ch.tick_num;
    ev.crafted.kind = piece.kind;
    ev.crafted.maker_id = id;
    ev.crafted.context = piece.context;
    ev.crafted.motif = piece.motif;   // the app mirrors the real form from this
    strlcpy(ev.crafted.who, name, sizeof(ev.crafted.who));
    strlcpy(ev.crafted.honoree, piece.honoree, sizeof(ev.crafted.honoree));
    ch.emit(ev);

    anim_type = LG_ANIM_NONE;
    state = STATE_IDLE;
    has_target = false;
    has_target_cell = false;
    afterglow_ticks = Cfg::AFTERGLOW_TICKS;   // making feels good
    idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
    _rest_muse();   // memorials rest it too — the grief exception is on entry
}

// Off to the allotment: walk to the claimed plot, lean over the soil for a
// few seconds, sow it. Bails gracefully if another farmer got there first
// (both may set out for the same plot — first one to arrive plants it).
void Conker::_do_farming(Chamber& ch) {
    zoomie_ticks--;   // repurposed as the trip's failsafe timer
    int plot = zoomie_target;
    bool plot_gone = plot < 0 || plot >= Cfg::GARDEN_PLOTS
                  || ch.plants[plot].stage != PLOT_SOIL;
    if (zoomie_ticks <= 0 || plot_gone || !ch.is_garden) {
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        sowing = false;
        anim_type = LG_ANIM_NONE;
        anim_remaining_ticks = 0;
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        return;
    }

    const Plant& p = ch.plants[plot];
    int cx = cell_x(), cy = cell_y();
    if (abs(p.x - cx) + abs(p.y - cy) > 1) {
        _step_toward_cell(p.x, p.y, ch);
        return;
    }

    // At the plot — settle into the sowing lean. The generic animation-freeze
    // block owns the countdown and clears anim_type the tick it completes, so
    // by the time we run again anim_type is already NONE — we can't use it to
    // tell "still sowing" from "just finished" (that bug looped the lean and
    // never sowed). Track our own progress with `sowing` instead.
    if (!sowing) {
        sowing = true;
        anim_type = LG_ANIM_GROOMING;
        anim_remaining_ticks = Cfg::SOW_DURATION_TICKS;
        float bdx = (p.x + 0.5f) - x, bdy = (p.y + 0.5f) - y;
        if (fabsf(bdx) >= fabsf(bdy)) {
            anim_lean_dx = (bdx >= 0) ? 1 : -1; anim_lean_dy = 0;
        } else {
            anim_lean_dx = 0; anim_lean_dy = (bdy >= 0) ? 1 : -1;
        }
        return;
    }
    if (anim_remaining_ticks > 0) return;   // freeze block still counting the lean down

    // Done — seed in the ground
    sowing = false;
    anim_type = LG_ANIM_NONE;
    anim_remaining_ticks = 0;
    if (ch.sow_plot(plot, id, name)) {
        Event ev = {};
        ev.type = EVT_CROP_SOWN;
        ev.tick = ch.tick_num;
        ev.crop.plot = (uint8_t)plot;
        ev.crop.sower_id = id;
        strlcpy(ev.crop.who, name, sizeof(ev.crop.who));
        ch.emit(ev);
    }
    state = STATE_IDLE;
    has_target = false;
    has_target_cell = false;
    idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
}

// Summoned to fill the vacant garden post: walk to the entry cell of the
// garden-ward face — the edge-crossing scan hands us over the moment we
// stand on it, and this state rides the transfer. On the garden side the
// next _pick_task claims the post. Fails soft back to idle if the face
// closes or the trip runs long.
void Conker::_do_to_garden(Chamber& ch) {
    zoomie_ticks--;   // repurposed: trip failsafe timer
    if (ch.is_garden) {                 // arrived — settle in, claim on repoll
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_ticks_remaining = 0;
        idle_repoll_tick = 0;
        return;
    }
    int face = zoomie_target;           // repurposed: face toward the garden
    bool face_ok = face >= 0 && face < FACE_COUNT && ch.entries[face] >= 0;
    if (zoomie_ticks <= 0 || !face_ok) {
        Serial.printf("[garden] %s abandons the trip (face=%d entries=%d ticks=%d)\r\n",
                      name, face,
                      (face >= 0 && face < FACE_COUNT) ? ch.entries[face] : -99,
                      zoomie_ticks);
        state = STATE_IDLE;
        has_target = false;
        has_target_cell = false;
        idle_repoll_tick = Cfg::IDLE_REPOLL_INTERVAL;
        return;
    }
    _step_toward_cell(Cfg::ENTRY_X[face], Cfg::ENTRY_Y[face], ch);
}

void Conker::_tick_idle(Chamber& ch) {
    idle_ticks_remaining--;
    idle_micro_ticks--;

    // Timer expired → exit idle
    if (idle_ticks_remaining <= 0) {
        speed = base_speed();
        has_target_cell = false;
        _pick_task(ch);
        return;
    }

    // A nearby glow is irresistible to a restless night idler — but only one
    // that isn't yet ready for bed (tired conkers head to sleep instead, so
    // chasing never keeps them up all night).
    if (g_tod.night_factor >= Cfg::FIREFLY_NIGHT_FACTOR_MIN
            && needs[NEED_REST] < Cfg::TIRED_SLEEP_NIGHT
            && !sleeping && stack_on < 0 && anim_type == LG_ANIM_NONE
            && g_rng.rand_float() < Cfg::FIREFLY_CHASE_CHANCE
                                    * (0.4f + 1.2f * personality[PERS_BRAVERY])) {
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

    // The muse strikes: with a deep pantry, a maker drifts off to craft —
    // something of this moment, in their own colours. Sometimes the muse
    // is a person: a best friend without a keepsake gets a gift instead.
    // Rested muse only (grief memorials are the one exception — see
    // _do_mourning); the roll scales with surplus, so a barely-deep pantry
    // inspires less than a groaning one.
    if (!sleeping && stack_on < 0 && anim_type == LG_ANIM_NONE
            && muse() >= Cfg::MUSE_MIN
            && g_tod.unix_time >= muse_rest_unix
            && ch.colony->play_surplus() > 0.4f
            && g_rng.rand_float() < Cfg::CRAFT_CHANCE_PER_TICK * muse()
                                    * ch.colony->play_surplus()) {
        uint32_t gift_for = 0;
        if (ch.bonds) {
            for (int i = 0; i < ch.conker_count; i++) {
                const Conker& o = ch.conkers[i];
                if (&o == this || !o.alive || o.accessory != 0) continue;
                if (_is_best_friend(ch, o.id)) { gift_for = o.id; break; }
            }
        }
        uint8_t kind;
        if (gift_for != 0 && g_rng.rand_float() < Cfg::CRAFT_GIFT_CHANCE) {
            kind = (uint8_t)(ART_HAT + g_rng.rand_int(0, 2));
            craft_for = gift_for;
        } else {
            kind = (personality[PERS_ROUTE_STICKINESS] > 0.6f) ? ART_CAIRN
                 : (personality[PERS_PLAYFULNESS] >= personality[PERS_EXPLORATION])
                    ? ART_SCULPTURE : ART_PAINTING;
            craft_for = 0;
        }
        if (_start_crafting(ch, kind, _craft_context_now(), cell_x(), cell_y()))
            return;
        craft_for = 0;
    }

    // Pause to admire a nearby work — taste runs on playfulness + curiosity.
    // Admiring warms the admirer toward the maker: art connects strangers.
    if (!sleeping && stack_on < 0 && anim_type == LG_ANIM_NONE) {
        float taste = 0.5f * personality[PERS_PLAYFULNESS]
                    + 0.5f * personality[PERS_EXPLORATION];
        if (g_rng.rand_float() < Cfg::ADMIRE_CHANCE_PER_TICK * taste) {
            int ai = ch.nearest_artwork(cell_x(), cell_y(), 4);
            if (ai >= 0 && ch.artworks[ai].maker_id != id) {
                const Artwork& a = ch.artworks[ai];
                anim_type = LG_ANIM_GROOMING;   // lean in for a look
                anim_remaining_ticks = Cfg::GREETING_DURATION_TICKS;
                float bdx = (a.x + 0.5f) - x, bdy = (a.y + 0.5f) - y;
                if (fabsf(bdx) >= fabsf(bdy)) {
                    anim_lean_dx = (bdx >= 0) ? 1 : -1; anim_lean_dy = 0;
                } else {
                    anim_lean_dx = 0; anim_lean_dy = (bdy >= 0) ? 1 : -1;
                }
                needs[NEED_BOREDOM] -= Cfg::ADMIRE_BOREDOM_RELIEF;
                if (needs[NEED_BOREDOM] < 0.0f) needs[NEED_BOREDOM] = 0.0f;
                if (ch.bonds)
                    ch.bonds->increment(id, a.maker_id, Cfg::ADMIRE_BOND_NUDGE);
                ch.artwork_admired(ai);
            }
        }
    }

    // (Sowing is no longer an idle roll — the posted gardener picks it up as
    // a job in _pick_task: carrying food > fulfilling need > gardening > idling.)

    // Joy sprint: a well-fed daytime idler sometimes just takes off — pure
    // high spirits, no partner needed. Bystanders may get swept into the
    // game via the proximity join. The curious ones launch most often.
    if (g_tod.phase == PHASE_DAY && !sleeping && stack_on < 0
            && anim_type == LG_ANIM_NONE && interaction_cooldown == 0) {
        // A full pantry frees time for play — but so does plain boredom: a
        // restless conker takes off even on an empty larder, just to burn it off.
        // v152: boredom only drives play once it crosses the conker's own
        // curiosity-set threshold (incurious ones let it ride longer).
        // v161: driven by the unified play desire (boredom×playfulness + surplus),
        // not a fixed exploration-scaled chance. The roll is only timing jitter.
        float urge = _play_desire(ch);
        if (urge > 0.0f
                && g_rng.rand_float() < Cfg::JOY_SPRINT_CHANCE * urge) {
            // Don't pile on: if a game is already running, watch instead
            int playing = 0;
            for (int i = 0; i < ch.conker_count; i++) {
                if (ch.conkers[i].alive && ch.conkers[i].state == STATE_ZOOMIES)
                    playing++;
            }
            if (playing >= Cfg::MAX_CONCURRENT_PLAY) return;
            state = STATE_ZOOMIES;
            zoomie_target = -1;  // runner
            zoomie_style = 0;
            zoomie_ticks = g_rng.rand_int(Cfg::ZOOMIE_MIN_TICKS,
                                          Cfg::ZOOMIE_MAX_TICKS
                                          + static_cast<int>(fminf(urge, 2.0f) * Cfg::ZOOMIE_SURPLUS_TICKS));
            has_target = false;
            has_target_cell = false;
            idle_ticks_remaining = 0;
            Serial.printf("[zoomies] %s takes off, just because\r\n", name);
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

// Evolve the needs drives and derive the display mood. The framework holds
// several needs but only those in Cfg::NEEDS_ACTIVE_MASK move — so each can be
// activated and tuned on its own. First active need: boredom.
void Conker::_update_needs(Chamber& ch, float dt) {
    // ---- Boredom (stimulation) ----
    if (Cfg::NEEDS_ACTIVE_MASK & (1 << NEED_BOREDOM)) {
        float& boredom = needs[NEED_BOREDOM];
        if (state == STATE_ZOOMIES) {
            boredom -= Cfg::BOREDOM_PLAY_DRAIN_PER_SEC * dt;   // playing relieves it
        } else if (sleeping || departing) {
            // leave it be
        } else if (g_tod.phase == PHASE_DAY) {
            // Boredom is a daytime drive — builds only during the active day.
            // Restless temperaments crave stimulation: busy bees, explorers
            // and social butterflies fill fast; placid loners barely climb.
            float drive = 0.4f
                        + 0.7f * personality[PERS_WORK_TEMPO]
                        + 0.6f * personality[PERS_EXPLORATION]
                        + 0.3f * personality[PERS_SOCIAL_FREQUENCY];
            bool working = (state == STATE_TO_FOOD || state == STATE_TO_HOME
                         || state == STATE_TEND_BROOD || state == STATE_TEND_QUEEN
                         || state == STATE_EATING);
            float rise = Cfg::BOREDOM_RISE_PER_SEC * drive * dt;
            if (working) rise *= Cfg::BOREDOM_WORK_RISE_SCALE;  // rote work helps a little
            boredom += rise;
        } else {
            // Dawn/dusk/night: wind down so restlessness never keeps them up.
            boredom -= Cfg::BOREDOM_NIGHT_DECAY_PER_SEC * dt;
        }
        if (boredom < 0.0f) boredom = 0.0f;
        if (boredom > 1.0f) boredom = 1.0f;
    }

    // ---- Tiredness (rest) ----
    if (Cfg::NEEDS_ACTIVE_MASK & (1 << NEED_REST)) {
        float& tired = needs[NEED_REST];
        if (sleeping) {
            tired -= Cfg::TIRED_FALL_PER_SEC * dt;             // a good sleep restores
        } else if (!departing) {
            // Awake is tiring; hardy ones last longer (staggers bedtimes).
            tired += Cfg::TIRED_RISE_PER_SEC * dt
                   * (1.2f - 0.4f * personality[PERS_HARDINESS]);
        }
        if (tired < 0.0f) tired = 0.0f;
        if (tired > 1.0f) tired = 1.0f;
    }

    // ---- Social (companionship) ----
    if (Cfg::NEEDS_ACTIVE_MASK & (1 << NEED_SOCIAL)) {
        float& lonely = needs[NEED_SOCIAL];
        // v165: nearby nestmates soothe loneliness — INCLUDING sleeping ones. A
        // conker among its dozing huddle-mates isn't alone (awake or asleep); only
        // one genuinely off by itself keeps getting lonelier. (Was: awake conkers
        // ignored sleepers, so they stayed maxed-lonely right next to the pile.)
        float company = _companions_near(ch, /*include_sleeping=*/true);
        if (departing) {
            // leave it be
        } else if (company > 0.0f) {
            // Company soothes — a friend's company more than a stranger's.
            lonely -= Cfg::SOCIAL_FALL_PER_SEC * dt * fminf(company, 3.0f);
        } else {
            // The sociable crave company; loners barely notice being alone.
            float drive = 0.5f + 1.0f * personality[PERS_SOCIAL_FREQUENCY];
            float rise = Cfg::SOCIAL_RISE_PER_SEC * drive * dt;
            if (sleeping) rise *= Cfg::SOCIAL_SLEEP_RISE_SCALE;  // climbs slower in sleep
            // A princess has no colony to be lonely *for* — on the colony curve
            // she pegs at 100 forever and reads as permanently miserable, which
            // is neither true to "you are her friend" nor tendable.
            if (ch.incubation_mode) rise *= Cfg::PRINCESS_SOCIAL_RISE_SCALE;
            lonely += rise;
        }
        if (lonely < 0.0f) lonely = 0.0f;
        if (lonely > 1.0f) lonely = 1.0f;
        // She tops out at "missing you", not despair — see PRINCESS_SOCIAL_PLATEAU.
        if (ch.incubation_mode && lonely > Cfg::PRINCESS_SOCIAL_PLATEAU)
            lonely = Cfg::PRINCESS_SOCIAL_PLATEAU;
    }

    // ---- Afterglow: topped up while playing, then decays into a content beat ----
    if (state == STATE_ZOOMIES) afterglow_ticks = Cfg::AFTERGLOW_TICKS;
    else if (afterglow_ticks > 0) afterglow_ticks--;

    // ---- Arbiter: loudest active need → intent_need → mood ----
    intent_need = NEED_COUNT;
    if (!sleeping) {
        float best = 0.0f;
        for (uint8_t n = 0; n < NEED_COUNT; n++) {
            if (!(Cfg::NEEDS_ACTIVE_MASK & (1 << n))) continue;
            float s = _need_salience(n, ch);
            if (s > best) { best = s; intent_need = n; }
        }
    }

    ConkerMood m = MOOD_CONTENT;
    if (state == STATE_ZOOMIES || state == STATE_PLAYING) {
        m = MOOD_PLAYING;
    } else if (sleeping || seeking_company) {
        m = MOOD_SLEEPY;   // v150: asleep, or groggily padding over to huddle up
    } else if (afterglow_ticks > 0) {
        m = MOOD_HAPPY;                          // just had a good time — savour it
    } else if (intent_need == NEED_REST) {
        float tired = needs[NEED_REST];
        bool nightish = (g_tod.phase != PHASE_DAY);
        if (tired >= _nap_threshold()
                || (tired >= Cfg::TIRED_SLEEP_NIGHT && nightish))
            m = MOOD_SLEEPY;
    } else if (intent_need == NEED_BOREDOM) {
        // v152: a conker reads "bored" at ITS OWN action threshold (curiosity-set),
        // "restless" approaching it — so the mood matches what each one actually
        // feels, not a one-size-fits-all line.
        float b = needs[NEED_BOREDOM];
        float act = _boredom_act_threshold();
        if (b >= act)              m = MOOD_BORED;
        else if (b >= act * 0.6f)  m = MOOD_RESTLESS;
    } else if (intent_need == NEED_SOCIAL) {
        float s = needs[NEED_SOCIAL];
        if (s >= Cfg::SOCIAL_URGENT_AT)         m = MOOD_LONELY;
        else if (s >= Cfg::SOCIAL_LONELY_AT)    m = MOOD_RESTLESS;
    }
    mood = static_cast<uint8_t>(m);
}

// ---- Needs framework helpers (v127) ----

// Context-gated salience for the arbiter. Boredom only matters in the active day;
// rest and (dormant) social matter whenever they're high.
float Conker::_need_salience(uint8_t need, Chamber& ch) const {
    (void)ch;
    float w = Cfg::NEED_SALIENCE_WEIGHT[need];
    switch (need) {
        case NEED_BOREDOM:
            return (g_tod.phase == PHASE_DAY) ? needs[NEED_BOREDOM] * w : 0.0f;
        case NEED_REST:   return needs[NEED_REST]   * w;
        case NEED_SOCIAL: return needs[NEED_SOCIAL] * w;
        default:          return 0.0f;
    }
}

// Chronotype: dozy conkers (low hardiness/tempo) nod off at a lower tiredness —
// so they take daytime naps — while hardy ones hold out toward night. Centred so
// an average conker keeps the default any-time sleep threshold.
float Conker::_nap_threshold() const {
    float nappiness = 0.6f * (0.5f - personality[PERS_HARDINESS])
                    + 0.4f * (0.5f - personality[PERS_WORK_TEMPO]);   // -0.5..+0.5
    float base = Cfg::TIRED_SLEEP_ANY - Cfg::CHRONO_NAP_RANGE * nappiness;  // 0.55..0.85
    // Night owls run a shifted clock: hard to send to bed while the
    // fireflies are out, quick to nap once the sun is up. Their existing
    // idle repertoire (drift, huddle, firefly chase) becomes the night shift.
    if (is_night_owl()) {
        if (g_tod.phase == PHASE_NIGHT || g_tod.phase == PHASE_DUSK)
            base += Cfg::OWL_NIGHT_THRESHOLD_LIFT;
        else if (g_tod.phase == PHASE_DAY)
            base -= Cfg::OWL_DAY_THRESHOLD_DROP;
    }
    return base;
}

// v152: how bored a conker must be before it bothers to act (play it off) — and
// before it reads "bored". Curiosity is the dial, mirroring how hardiness sets
// the nap threshold: exploration 0 → only at HIGH (0.80, lets it ride), 1 → LOW
// (0.20, twitchy). So the incurious visibly stew while the curious keep busy.
float Conker::_boredom_act_threshold() const {
    float curiosity = personality[PERS_EXPLORATION];
    return Cfg::BOREDOM_ACT_HIGH
         - (Cfg::BOREDOM_ACT_HIGH - Cfg::BOREDOM_ACT_LOW) * curiosity;
}

// Unified play desire (0..~3): boredom past the conker's own curiosity threshold,
// scaled by innate PLAYFULNESS, plus a full-pantry festival bonus. 0 when content.
// The driven replacement for the old fixed play "chances" — callers multiply this
// by a small per-tick rate and roll only for timing jitter.
float Conker::_play_desire(Chamber& ch) const {
    float bored_urge = (needs[NEED_BOREDOM] >= _boredom_act_threshold())
                     ? needs[NEED_BOREDOM] : 0.0f;
    float playful = 0.4f + personality[PERS_PLAYFULNESS];   // 0.4 .. 1.4
    return bored_urge * playful * Cfg::BOREDOM_PLAY_DRIVE
         + ch.colony->play_surplus() * (0.3f + personality[PERS_PLAYFULNESS]);
}

// Unified social desire (0..~2): an innate sociability baseline plus a loneliness-
// need boost. A lonely social butterfly actively seeks contact; a content loner
// doesn't bother. Drives the proximity greet/groom/huddle/stack behaviours.
float Conker::_social_desire(Chamber& ch) const {
    (void)ch;
    float social = personality[PERS_SOCIAL_FREQUENCY];
    return social * 0.6f + needs[NEED_SOCIAL] * (0.3f + social);
}

// Should a sleeping conker wake this tick? Famine/hunger always rouses (a player
// boop wakes directly, in sim.cpp). A NIGHT sleep holds through the dark — it
// never pops awake at 3am — and gets up at dawn once rested, or is forced up by
// full day. A daytime NAP is a short top-up: up once it's restored to its target,
// not drained to empty like a night's sleep.
bool Conker::_should_wake(Chamber& ch) const {
    // Incubation/princess: she can only eat food the keeper dropped, so hunger
    // rouses her ONLY when a pile is actually there to toddle to. Without this
    // gate an unfed sleeping princess spins wake→(nothing to eat)→sleep every
    // repoll, so sleep never accrues and tiredness pins at max ("always sleepy",
    // feedback #51). Neglect = sleep; dropped food = the wake (same rule as the
    // dormant rouse above in tick()).
    if (ch.incubation_mode) {
        if (ch.food_pile_count > 0 && hunger > Cfg::PRINCESS_EAT_FLOOR)
            return true;
    } else if (ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE
               || hunger > 60.0f) {
        return true;
    }
    if (daytime_nap)
        return needs[NEED_REST] <= nap_wake_target;
    if (g_tod.phase == PHASE_DAY) return true;   // never sleep through the working day
    if (_wants_company_wake(ch)) return true;    // v150: friendly + lonely + alone → go huddle
    return (g_tod.phase == PHASE_DAWN
            && needs[NEED_REST] <= Cfg::TIRED_MORNING_WAKE);
}

// v150: would this conker rouse from a night sleep to go find company? Only night
// sleeps (not naps/day), only the sociable (loners below SOCIAL_WAKE_MIN sleep
// alone all night untroubled), only when genuinely alone (no one — awake OR
// asleep — within reach), and only once loneliness clears a personality-scaled
// bar: the friendlier the conker, the LOWER the bar, so butterflies rouse sooner.
bool Conker::_wants_company_wake(Chamber& ch) const {
    if (daytime_nap || g_tod.phase == PHASE_DAY) return false;
    if (ch.incubation_mode) return false;   // nobody to huddle with — see _wants_company_awake
    // Survival first: a hunger/famine wake goes to EAT, not to huddle.
    if (hunger > 60.0f || ch.colony->food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE)
        return false;
    float social = personality[PERS_SOCIAL_FREQUENCY];
    if (social < Cfg::SOCIAL_WAKE_MIN) return false;
    float wake_at = Cfg::SOCIAL_WAKE_BASE - Cfg::SOCIAL_WAKE_SLOPE * social;
    if (needs[NEED_SOCIAL] < wake_at) return false;
    return _companions_near(ch, /*include_sleeping=*/true) <= 0.0f;
}

// v163: the AWAKE counterpart of the night huddle-wake — a lonely, sociable conker
// that's ended up alone wanders over to company (relieved by proximity), so the
// loneliness need drives behaviour by day too, not only from night sleep. Same
// sociability gate + threshold as the night wake. Loners keep to themselves.
bool Conker::_wants_company_awake(Chamber& ch) const {
    if (sleeping || departing) return false;
    // A princess is alone by construction — there is no one to amble over to, so
    // seeking company burns SOCIAL_SEEK_TIMEOUT_TICKS (150s) of her tick going
    // nowhere, over and over, once she's lonely. _tick_seek_company owns the tick
    // and returns before the decision phase, so while it runs she ignores food,
    // the ball, everything. Her company is the keeper, not a nestmate.
    // (Same family as the return-home and forager guards above.)
    if (ch.incubation_mode) return false;
    // Priority: survival (eat) and rest (sleep) outrank socialising — a hungry or
    // sleepy conker handles that first (via _pick_task) instead of wandering off.
    if (hunger > 40.0f) return false;
    if (needs[NEED_REST] >= _nap_threshold()) return false;
    // v165: no hard loner cutoff — the threshold below already makes low-social
    // conkers hold out far longer (seek_at ~0.95 at social 0), so even a hermit
    // ambles over once genuinely maxed, but only then.
    float social = personality[PERS_SOCIAL_FREQUENCY];
    float seek_at = Cfg::SOCIAL_WAKE_BASE - Cfg::SOCIAL_WAKE_SLOPE * social;
    if (needs[NEED_SOCIAL] < seek_at) return false;
    // v165: sleepers count as company — don't seek if nestmates are already here.
    return _companions_near(ch, /*include_sleeping=*/true) <= 0.0f;
}

// v150/v163: an amble toward the nearest friend (else nearest anyone, else the
// queen). Self-contained: sets a step target and advances, so it doesn't fight the
// idle state machine. Stops on arrival (got company) or timeout — then a NIGHT
// seeker nestles back to sleep, while a daytime seeker just returns to normal.
void Conker::_tick_seek_company(Chamber& ch) {
    state = STATE_IDLE;
    bool company = _companions_near(ch, /*include_sleeping=*/true) > 0.0f;
    if (company || seek_ticks == 0) {             // v163: no longer cancels at day — keep seeking by day too
        seeking_company = false;
        has_target_cell = false;
        if (g_tod.phase != PHASE_DAY) {           // nestle back down for the night
            sleeping = true;
            anim_type = LG_ANIM_SNOOZE;
            idle_ticks_remaining = g_rng.rand_int(Cfg::IDLE_REST_MIN_TICKS,
                                                  Cfg::IDLE_REST_MAX_TICKS);
        }
        return;                                   // (day: fall back to normal next tick)
    }
    if (seek_ticks > 0) seek_ticks--;
    speed = Cfg::IDLE_DRIFT_SPEED;

    int cx = cell_x(), cy = cell_y();
    int best = 999, tx = -1, ty = -1;
    bool found_friend = false;
    for (int i = 0; i < ch.conker_count; i++) {
        const Conker& o = ch.conkers[i];
        if (&o == this || !o.alive) continue;
        bool fr = _is_friend(ch, o.id);
        if (found_friend && !fr) continue;            // once we've spotted a friend, only friends count
        if (fr && !found_friend) { found_friend = true; best = 999; }  // restrict ranking to friends
        int d = abs(o.cell_x() - cx) + abs(o.cell_y() - cy);
        if (d > 0 && d < best) { best = d; tx = o.cell_x(); ty = o.cell_y(); }
    }
    if (tx < 0 && ch.has_queen && ch.queen_obj.alive) { tx = ch.queen_obj.x; ty = ch.queen_obj.y; }
    if (tx >= 0) {
        int dx = (tx > cx) ? 1 : ((tx < cx) ? -1 : 0);
        int dy = (ty > cy) ? 1 : ((ty < cy) ? -1 : 0);
        if (dx != 0 && dy != 0) { if (g_rng.rand_float() < 0.5f) dy = 0; else dx = 0; }
        _set_target_cell(cx + dx, cy + dy, ch);
    }
    _advance_toward_target(ch);
}

// Personality-flavoured response to a need. Routing hook for the arbiter — the
// boredom/rest styles are reachable now; social is scaffolded for when it's lit.
uint8_t Conker::_response_style(uint8_t need) const {
    switch (need) {
        case NEED_BOREDOM: {
            float social  = personality[PERS_SOCIAL_FREQUENCY];
            float explore = personality[PERS_EXPLORATION];
            if (social > 0.6f && social >= explore) return RESP_PLAY_SOCIAL;
            if (explore > 0.45f)                    return RESP_PLAY_SOLO;
            return RESP_FIDGET;
        }
        case NEED_REST:   return RESP_SLEEP;
        case NEED_SOCIAL: return RESP_SEEK_COMPANY;
        default:          return RESP_NONE;
    }
}

// I've bonded to this conker (one-way is enough). Best friend = mutual.
bool Conker::_is_friend(Chamber& ch, uint32_t other_id) const {
    return ch.bonds && ch.bonds->is_formed(id, other_id);
}
bool Conker::_is_best_friend(Chamber& ch, uint32_t other_id) const {
    return ch.bonds && ch.bonds->is_formed(id, other_id)
                    && ch.bonds->is_formed(other_id, id);
}

// While casting about for food, a forager sometimes drifts toward a friend who's
// also out foraging — so friends end up working the same patch and reinforcing
// the same scent trails together. Returns true if it took over the step.
bool Conker::_forage_follow_friend(Chamber& ch) {
    if (!ch.bonds) return false;
    if (g_rng.rand_float() >= Cfg::BOND_FORAGE_FOLLOW) return false;
    int cx = cell_x(), cy = cell_y();
    int best = 999, tx = -1, ty = -1;
    for (int i = 0; i < ch.conker_count; i++) {
        const Conker& o = ch.conkers[i];
        if (&o == this || !o.alive || o.state != STATE_TO_FOOD) continue;
        if (!_is_friend(ch, o.id)) continue;
        int d = abs(o.cell_x() - cx) + abs(o.cell_y() - cy);
        if (d > 1 && d < best) { best = d; tx = o.cell_x(); ty = o.cell_y(); }
    }
    if (tx < 0) return false;
    _step_toward_cell(tx, ty, ch);
    return true;
}

// Company score from awake neighbours in range — a friend is worth more than a
// stranger, a best friend more still, so loneliness is soothed by WHO is near,
// not just how many.
float Conker::_companions_near(Chamber& ch, bool include_sleeping) const {
    float score = 0.0f;
    float r2 = Cfg::SOCIAL_COMPANION_DIST * Cfg::SOCIAL_COMPANION_DIST;
    for (int i = 0; i < ch.conker_count; i++) {
        const Conker& o = ch.conkers[i];
        if (&o == this || !o.alive) continue;
        if (o.sleeping && !include_sleeping) continue;
        float dx = o.x - x, dy = o.y - y;
        if (dx * dx + dy * dy > r2) continue;
        score += _is_best_friend(ch, o.id) ? 3.0f
               : _is_friend(ch, o.id)      ? 2.0f
                                           : 1.0f;
    }
    return score;
}

void Conker::_pick_idle_microstate(Chamber& ch) {
    has_target_cell = false;

    // Pirouette: a well-fed (or just restless) daytime idler sometimes
    // twirls. The curious ones most of all. (v152: boredom drives it only past
    // the conker's own curiosity-set action threshold.)
    float bored_urge = (needs[NEED_BOREDOM] >= _boredom_act_threshold())
                     ? needs[NEED_BOREDOM] * Cfg::BOREDOM_PLAY_DRIVE : 0.0f;
    float urge = ch.colony->play_surplus() + bored_urge;
    if (urge > 0.0f && g_tod.phase == PHASE_DAY && !sleeping
            && g_rng.rand_float() < Cfg::PIROUETTE_CHANCE * urge
                                    * (0.5f + personality[PERS_EXPLORATION])) {
        idle_microstate = 5;
        speed = base_speed();
        idle_micro_ticks = Cfg::PIROUETTE_TICKS;
        return;
    }

    // Personality steers the pick: placid low-tempo conkers hold still,
    // explorers drift, social butterflies huddle (feedback: personalities
    // should show in what they choose to do)
    float hold_w   = Cfg::IDLE_HOLD_WEIGHT   * (1.5f - personality[PERS_WORK_TEMPO]);
    float drift_w  = Cfg::IDLE_DRIFT_WEIGHT  * (0.5f + personality[PERS_EXPLORATION]);
    // Loneliness pushes toward huddling — and the huddle drift seeks out friends
    // (see _tick_idle microstate 3), so a lonely conker goes looking for company.
    float huddle_w = Cfg::IDLE_HUDDLE_WEIGHT
                   * (0.5f + personality[PERS_SOCIAL_FREQUENCY] + needs[NEED_SOCIAL]);
    float reface_w = 1.0f - (Cfg::IDLE_HOLD_WEIGHT + Cfg::IDLE_DRIFT_WEIGHT
                             + Cfg::IDLE_HUDDLE_WEIGHT);
    float r = g_rng.rand_float() * (hold_w + drift_w + huddle_w + reface_w);
    if (r < hold_w) {
        idle_microstate = 0;  // hold
        speed = base_speed();
    } else if (r < hold_w + drift_w) {
        idle_microstate = 1;  // random drift
        speed = Cfg::IDLE_DRIFT_SPEED;
    } else if (r < hold_w + drift_w + huddle_w) {
        idle_microstate = 3;  // huddle: drift toward nearest idler or queen
        speed = Cfg::IDLE_DRIFT_SPEED;
    } else {
        idle_microstate = 2;  // reface
        speed = base_speed();
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
            || state == STATE_MOURNING || state == STATE_FARMING
            || state == STATE_CRAFTING || state == STATE_TO_GARDEN
            || state == STATE_PLAYING)   // omit and the chase is re-tasked away
                                         // every tick — the v177 farming bug
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
    float surplus  = ch.colony->play_surplus();
    float frac = Cfg::BASE_FORAGER_FRACTION
        + (1.0f - Cfg::BASE_FORAGER_FRACTION)
          * (pressure / Cfg::FAMINE_SLOWDOWN_PRESSURE);
    // A deep pantry winds the commute down — the freed hands go play
    frac *= 1.0f - Cfg::SURPLUS_FORAGE_DAMP * surplus;
    // Waggle recruitment: a fresh delivery advertises a productive source.
    // The boost rides food_delivery_signal, so sustained deliveries hold the
    // swarm open and it dissolves as the signal decays. Recruits navigate by
    // the laid trail gradient — nothing is steered directly. When the pantry
    // is full the advert falls on deaf ears (breaks the forage feedback loop).
    frac += Cfg::RECRUIT_SIGNAL_FRACTION * (ch.food_delivery_signal / 200.0f)
          * (1.0f - surplus);
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

// ================================================================
//  Current activity — one readable "what are they doing right now"
// ================================================================

ConkerActivity conker_activity(const Conker& w, const Chamber& ch) {
    // Sleep and the lonely-amble sit on top of whatever state is underneath.
    if (w.sleeping)         return w.daytime_nap ? ACT_NAPPING : ACT_SLEEPING;
    if (w.seeking_company)  return ACT_SEEKING_COMPANY;

    switch (w.state) {
        case STATE_TO_FOOD:     return ACT_FORAGING;
        case STATE_TO_HOME:     return w.food_carried > 0 ? ACT_CARRYING_FOOD
                                                          : ACT_HEADING_HOME;
        case STATE_EATING:      return ACT_EATING;
        case STATE_ZOOMIES:
            if (w.zoomie_target <= -2) return ACT_CHASING_FIREFLY;  // firefly chase
            return (w.zoomie_style == 1) ? ACT_PARADING : ACT_PLAYING;
        case STATE_MOURNING:    return ACT_MOURNING;
        case STATE_FARMING:     return ACT_SOWING;      // on a sow trip / at the plot
        case STATE_CRAFTING:    return ACT_CRAFTING;
        case STATE_TO_GARDEN:   return ACT_TO_GARDEN;
        case STATE_TEND_BROOD:  return ACT_TENDING_BROOD;
        case STATE_TEND_QUEEN:  return ACT_FEEDING_QUEEN;
        case STATE_CANNIBALIZE: return ACT_CLEARING;
        case STATE_IDLE:
        default:
            // A green thumb holding the post but with nothing to sow is minding
            // the garden, not merely idling.
            if (ch.is_garden && ch.posted_gardener == w.id) return ACT_GARDENING;
            return ACT_IDLING;
    }
}

// Indexed by ConkerActivity — keep in lockstep with the enum.
static const char* const ACTIVITY_KEY[ACT_COUNT] = {
    "idling", "sleeping", "napping", "seeking_company", "foraging",
    "carrying_food", "heading_home", "eating", "chasing_firefly", "playing",
    "parading", "sowing", "gardening", "heading_to_garden", "tending_brood",
    "feeding_queen", "crafting", "mourning", "clearing", "away",
};
static const char* const ACTIVITY_SHORT[ACT_COUNT] = {
    "Pottering about", "Sleeping", "Napping", "Finding company", "Foraging",
    "Carrying food", "Heading home", "Eating", "Firefly chase", "Playing chase",
    "Parading", "Gardening", "Minding garden", "Off to garden", "Tending brood",
    "Feeding queen", "Crafting", "Mourning", "Tidying up", "Away",
};

const char* conker_activity_key(ConkerActivity a) {
    return (a < ACT_COUNT) ? ACTIVITY_KEY[a] : "away";
}
const char* conker_activity_short(ConkerActivity a) {
    return (a < ACT_COUNT) ? ACTIVITY_SHORT[a] : "Away";
}
