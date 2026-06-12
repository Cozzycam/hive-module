/* Queen behavior — real-time lifecycle. */
#include "queen.h"
#include "chamber.h"
#include "events.h"
#include "rng.h"
#include <Arduino.h>
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
    // Founder bootstrap: keep using founding cadence until all founder eggs are laid,
    // even after the first worker hatches (which sets founding_done = true)
    if (eggs_laid < Cfg::FOUNDER_COHORT_SIZE) {
        _lay_founding(ch, dt);
    } else if (founding_done) {
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
        if (b.food_invested >= Cfg::SEED_TOTAL_FOOD) continue;
        // Founding phase: feed proactively every tick (no hunger gate)
        // Accelerated brood can't wait hours for hunger to build up
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

// Eggs gather in a clutch beside the queen — a tight ring around one nest
// hollow so multiple eggs read as a single nest rather than scattered dots
// (Amber: eggs should pop and cluster; a nursery module role can take this
// over properly later).
static void _clutch_spot(int qx, int qy, int index, int& ex, int& ey) {
    static const int8_t OFF_X[] = { 0, 1, 0, 1, -1,  0,  1, -1, 2 };
    static const int8_t OFF_Y[] = { 0, 0, 1, 1,  0, -1, -1,  1, 0 };
    int nx = qx + 3, ny = qy + 2;  // nest hollow just off her right shoulder
    if (nx > Cfg::GRID_WIDTH - 3)  nx = Cfg::GRID_WIDTH - 3;
    if (ny > Cfg::GRID_HEIGHT - 3) ny = Cfg::GRID_HEIGHT - 3;
    int k = index % 9;
    ex = nx + OFF_X[k];
    ey = ny + OFF_Y[k];
}

void Queen::_lay_founding(Chamber& ch, float dt) {
    if (eggs_laid >= Cfg::FOUNDER_COHORT_SIZE) {
        // All founding eggs laid, pause until first worker hatches
        return;
    }

    // Hatch-gated cadence: one founder at a time. Wait for any existing
    // founder brood to fully hatch before laying next. Also guards against
    // eggs_laid being stale after reboot (manifest flush is periodic).
    for (int i = 0; i < ch.brood_count; i++) {
        if (ch.brood[i].alive() && ch.brood[i].total_duration_ms > 0)
            return;  // previous founder still developing
    }

    // Lay one founding egg
    if (ch.brood_count >= Cfg::MAX_BROOD) return;
    float consumed = _consume(ch, Cfg::EGG_FOOD_COST);
    if (consumed < Cfg::EGG_FOOD_COST) {
        if (consumed > 0) reserves += consumed;
        return;
    }
    // Place in the clutch beside the queen
    int ex, ey;
    _clutch_spot(x, y, eggs_laid, ex, ey);
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

        // Place in the clutch beside the queen
        int ex, ey;
        _clutch_spot(x, y, eggs_laid, ex, ey);
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
