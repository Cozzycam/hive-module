/* Queen behavior — real-time lifecycle. */
#include "queen.h"
#include "chamber.h"
#include "events.h"
#include "rng.h"
#include <cmath>

void Queen::init(int8_t px, int8_t py) {
    x = px; y = py;
    eggs_laid = 0;
    hunger = 0.0f;
    alive = true;
    reserves = Cfg::FOOD_STORE_START;
    founding_done = false;
    egg_accum = 0.0f;
}

void Queen::tick(Chamber& ch, float dt) {
    if (!alive) return;

    // ---- Hunger ----
    // Hunger rises continuously; workers feed queen to reset it.
    // During founding, queen feeds herself from reserves.
    hunger += Cfg::QUEEN_HUNGER_PER_DAY / Cfg::SECS_PER_DAY * dt;
    if (!founding_done && reserves > 0.0f && hunger >= 30.0f) {
        float meal = fminf(Cfg::QUEEN_MEAL_COST, reserves);
        reserves -= meal;
        hunger = 0.0f;
    }
    if (hunger >= Cfg::HUNGER_STARVE) { alive = false; return; }

    // ---- Founding brood care ----
    if (!founding_done) {
        _tend_founding_brood(ch, dt);
    } else if (reserves > 0.0f) {
        // Dump remaining reserves into food store on transition
        ch.colony->food_store += reserves;
        reserves = 0.0f;
    }

    // ---- Egg laying ----
    if (!founding_done) {
        _lay_founding(ch, dt);
    } else {
        _lay_established(ch, dt);
    }
}

float Queen::_consume(Chamber& ch, float amount) {
    if (reserves >= amount) { reserves -= amount; return amount; }
    float from_reserves = reserves;
    reserves = 0.0f;
    float remainder = amount - from_reserves;
    float store = ch.colony->food_store;
    if (store >= remainder) {
        ch.colony->food_store -= remainder;
        return amount;
    }
    ch.colony->food_store = fmaxf(0.0f, store - remainder);
    return from_reserves + store;
}

void Queen::_tend_founding_brood(Chamber& ch, float dt) {
    // Queen feeds larvae from her reserves during founding
    float feed_per_larva = Cfg::LARVA_FOOD_PER_DAY / Cfg::SECS_PER_DAY * dt;
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_SEED || !b.alive()) continue;
        if (b.food_invested >= Cfg::LARVA_TOTAL_FOOD) continue;
        if (!b.needs_feeding()) continue;
        float consumed = _consume(ch, feed_per_larva);
        if (consumed > 0) {
            b.hunger = 0.0f;
            b.food_invested += consumed;
        }
    }
}

bool Queen::_can_lay(Chamber& ch) {
    if (ch.colony->food_total < Cfg::QUEEN_LAY_FOOD_FLOOR) return false;

    int pop = ch.colony->population;
    if (pop > 0) {
        int pending = ch.colony->brood_egg + ch.colony->brood_larva;
        int cap = pop * Cfg::QUEEN_MAX_BROOD_RATIO;
        if (cap < 4) cap = 4;
        if (pending >= cap) return false;
    }
    return true;
}

void Queen::_lay_founding(Chamber& ch, float dt) {
    if (eggs_laid >= Cfg::FOUNDER_COHORT_SIZE) {
        // All founding eggs laid, pause until first worker hatches
        return;
    }

    // Hatch-gated cadence: lay egg 0 immediately, then wait for
    // previous founding brood to fully hatch before laying next.
    if (eggs_laid > 0) {
        // Check if any founder brood still exists (egg or seed stage)
        for (int i = 0; i < ch.brood_count; i++) {
            if (ch.brood[i].alive() && ch.brood[i].total_duration_ms > 0)
                return;  // previous founder still developing
        }
    }

    // Lay one founding egg
    if (ch.brood_count >= Cfg::MAX_BROOD) return;
    float consumed = _consume(ch, Cfg::EGG_FOOD_COST);
    if (consumed < Cfg::EGG_FOOD_COST) {
        if (consumed > 0) reserves += consumed;
        return;
    }
    // Place around queen, just outside her sprite (~3 cells from center)
    int dx, dy;
    for (int att = 0; att < 20; att++) {
        dx = g_rng.rand_int(-4, 4);
        dy = g_rng.rand_int(-4, 4);
        if (abs(dx) + abs(dy) >= 3 && (abs(dx) > 2 || abs(dy) > 2))
            break;
    }
    int ex = x + dx, ey = y + dy;
    if (ch.in_bounds(ex, ey)) {
        ch.add_brood_with_duration(ex, ey, Cfg::FOUNDER_DURATIONS_MS[eggs_laid]);
        eggs_laid++;
        Event ev; ev.type = EVT_QUEEN_LAID_EGG; ev.tick = ch.tick_num;
        ev.position = {static_cast<int8_t>(ex), static_cast<int8_t>(ey)};
        ch.emit(ev);
    } else {
        reserves += consumed;  // refund, try next tick
    }
}

void Queen::_lay_established(Chamber& ch, float dt) {
    if (!_can_lay(ch)) return;
    if (ch.colony->food_store < Cfg::EGG_FOOD_COST) return;

    int pop = ch.colony->population;
    float pop_factor = fminf(1.0f, static_cast<float>(pop) / Cfg::LAY_RATE_POP_SCALE);
    float base_rate = Cfg::ESTABLISHED_LAY_RATE_BASE
                    + (Cfg::ESTABLISHED_LAY_RATE_MAX - Cfg::ESTABLISHED_LAY_RATE_BASE) * pop_factor;

    float pressure = ch.colony->food_pressure();
    float pressure_mult = 1.0f;
    if (pressure > Cfg::QUEEN_LAY_PRESSURE_MAX) {
        float t = (pressure - Cfg::QUEEN_LAY_PRESSURE_MAX)
                / (1.0f - Cfg::QUEEN_LAY_PRESSURE_MAX);
        pressure_mult = 1.0f - t * (1.0f - Cfg::LAY_PRESSURE_FLOOR);
    }

    float effective_rate = base_rate * pressure_mult;
    egg_accum += effective_rate / Cfg::SECS_PER_DAY * dt;

    while (egg_accum >= 1.0f) {
        if (ch.brood_count >= Cfg::MAX_BROOD) break;
        if (ch.colony->food_store < Cfg::EGG_FOOD_COST) break;

        ch.colony->food_store -= Cfg::EGG_FOOD_COST;

        // Place around queen, just outside her sprite (~3 cells from center)
        int dx, dy;
        for (int att = 0; att < 20; att++) {
            dx = g_rng.rand_int(-4, 4);
            dy = g_rng.rand_int(-4, 4);
            if (abs(dx) + abs(dy) >= 3 && (abs(dx) > 2 || abs(dy) > 2))
                break;
        }
        int ex = x + dx, ey = y + dy;
        if (ch.in_bounds(ex, ey)) {
            ch.add_brood(ex, ey, Cfg::DEFAULT_BROOD_ROLE);
            eggs_laid++;
            Event ev; ev.type = EVT_QUEEN_LAID_EGG; ev.tick = ch.tick_num;
            ev.position = {static_cast<int8_t>(ex), static_cast<int8_t>(ey)};
            ch.emit(ev);
        } else {
            ch.colony->food_store += Cfg::EGG_FOOD_COST; // refund
        }
        egg_accum -= 1.0f;
    }
}
