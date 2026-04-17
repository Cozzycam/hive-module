/* Queen behavior — ported from sim/queen.py. */
#include "queen.h"
#include "chamber.h"
#include "events.h"
#include "rng.h"
#include <cmath>

void Queen::init(int8_t px, int8_t py) {
    x = px; y = py;
    lay_cooldown = Cfg::QUEEN_LAY_INTERVAL_FOUNDING;
    eggs_laid = 0;
    hunger = 0.0f;
    alive = true;
    reserves = Cfg::FOOD_STORE_START;
}

void Queen::tick(Chamber& ch) {
    if (!alive) return;

    // Metabolism
    float consumed = _consume(ch, Cfg::QUEEN_METABOLISM);
    if (consumed > 0) {
        if (hunger > 0) hunger = fmaxf(0.0f, hunger - Cfg::QUEEN_METABOLISM);
    } else {
        hunger += Cfg::QUEEN_HUNGER_RATE;
        if (hunger >= Cfg::QUEEN_STARVE_THRESHOLD) { alive = false; return; }
    }

    // Founding brood care
    if (ch.colony->population == 0) {
        _tend_founding_brood(ch);
    } else if (reserves > 0) {
        ch.colony->food_store += reserves;
        reserves = 0.0f;
    }

    // Egg laying
    if (lay_cooldown > 0) { lay_cooldown--; return; }

    bool founding = ch.colony->population == 0;
    if (_can_lay(ch)) {
        _lay(ch);
        if (founding)
            lay_cooldown = Cfg::QUEEN_LAY_INTERVAL_FOUNDING;
        else {
            float pressure = ch.colony->food_pressure();
            int base = Cfg::QUEEN_LAY_INTERVAL_NORMAL;
            lay_cooldown = (pressure > Cfg::QUEEN_LAY_SLOWDOWN) ? base * 2 : base;
        }
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

void Queen::_tend_founding_brood(Chamber& ch) {
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (b.stage != STAGE_LARVA || !b.alive()) continue;
        if (b.fed_total >= b.food_needed) continue;
        if (!b.needs_feeding()) continue;
        if (abs(b.x - x) + abs(b.y - y) <= 3) {
            float consumed = _consume(ch, Cfg::LARVA_FEED_AMOUNT);
            if (consumed > 0) b.feed(consumed);
            return;
        }
    }
}

bool Queen::_can_lay(Chamber& ch) {
    float pressure = ch.colony->food_pressure();
    if (pressure > Cfg::QUEEN_LAY_PRESSURE_MAX) return false;
    if (ch.colony->food_total < Cfg::QUEEN_LAY_FOOD_FLOOR) return false;

    bool founding = ch.colony->population == 0;
    if (founding && eggs_laid >= Cfg::QUEEN_FOUNDING_EGG_CAP) return false;

    if (!founding) {
        int pending = ch.colony->brood_egg + ch.colony->brood_larva;
        int cap = ch.colony->population * Cfg::QUEEN_MAX_BROOD_RATIO;
        if (cap < 4) cap = 4;
        if (pending >= cap) return false;

        int batch = ch.colony->population / 5;
        if (batch < 1) batch = 1;
        if (batch > 6) batch = 6;
        float brood_cost = batch * Cfg::ROLE_PARAMS[Cfg::DEFAULT_BROOD_ROLE].larva_food_needed;
        if (ch.colony->food_total < brood_cost + Cfg::QUEEN_LAY_FOOD_FLOOR)
            return false;
    }
    return true;
}

void Queen::_lay(Chamber& ch) {
    int max_batch;
    if (ch.colony->population == 0) {
        max_batch = 6;
    } else {
        int base_batch = ch.colony->population / 5;
        if (base_batch < 1) base_batch = 1;
        if (base_batch > 6) base_batch = 6;
        float pressure = ch.colony->food_pressure();
        float comfort = Cfg::QUEEN_LAY_PRESSURE_MAX * 0.33f;
        if (pressure <= comfort) {
            max_batch = base_batch;
        } else {
            float t = (pressure - comfort) / (Cfg::QUEEN_LAY_PRESSURE_MAX - comfort);
            max_batch = static_cast<int>(roundf(base_batch * (1.0f - t)));
            if (max_batch < 1) max_batch = 1;
        }
    }

    for (int i = 0; i < max_batch; i++) {
        float consumed = _consume(ch, Cfg::QUEEN_EGG_FOOD_COST);
        if (consumed < Cfg::QUEEN_EGG_FOOD_COST) {
            if (consumed > 0) reserves += consumed;
            return;
        }
        int dx = g_rng.rand_int(-2, 2);
        int dy = g_rng.rand_int(-2, 2);
        int ex = x + dx, ey = y + dy;
        if (ch.in_bounds(ex, ey) && (dx != 0 || dy != 0)) {
            ch.add_brood(ex, ey, Cfg::DEFAULT_BROOD_ROLE);
            eggs_laid++;
            Event ev; ev.type = EVT_QUEEN_LAID_EGG; ev.tick = ch.tick_num;
            ch.emit(ev);
        } else {
            reserves += consumed;
        }
    }
}
