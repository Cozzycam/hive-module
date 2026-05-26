#include "brood.h"
#include <Arduino.h>

void Brood::init(int8_t px, int8_t py, Role c) {
    x = px;
    y = py;
    stage = STAGE_EGG;
    role = c;
    stage_start_ms = millis();
    total_duration_ms = 0;  // use default durations
    hunger = 0.0f;
    food_invested = 0.0f;
}

void Brood::init_with_duration(int8_t px, int8_t py, uint32_t duration_ms) {
    init(px, py, ROLE_CONKER);
    total_duration_ms = duration_ms;
}

BroodTransition Brood::tick(float dt) {
    if (stage == STAGE_DEAD) return BROOD_NONE;

    uint32_t elapsed_ms = millis() - stage_start_ms;

    // Compute per-stage durations
    // If total_duration_ms is set (founder bootstrap), egg = 1/3, seed = 2/3
    // Otherwise use default constants
    uint32_t egg_dur_ms, seed_dur_ms;
    if (total_duration_ms > 0) {
        egg_dur_ms  = total_duration_ms / 3;
        seed_dur_ms = total_duration_ms - egg_dur_ms;
    } else {
        egg_dur_ms  = static_cast<uint32_t>(Cfg::EGG_DURATION_DAYS * Cfg::SECS_PER_DAY * 1000.0f);
        seed_dur_ms = static_cast<uint32_t>(Cfg::SEED_DURATION_DAYS * Cfg::SECS_PER_DAY * 1000.0f);
    }

    if (stage == STAGE_EGG) {
        if (elapsed_ms >= egg_dur_ms) {
            stage = STAGE_SEED;
            stage_start_ms = millis();
            hunger = 0.0f;
            return BROOD_EGG_TO_SEED;
        }
        return BROOD_NONE;
    }

    if (stage == STAGE_SEED) {
        // Hunger accumulates in real-time (normalized 0-1 over ~1 day without feeding)
        hunger += dt / Cfg::SECS_PER_DAY;
        if (hunger >= 1.0f) {
            stage = STAGE_DEAD;
            return BROOD_DIED;
        }
        // Founder brood (accelerated): time-only hatch, no food gate
        // Normal brood: requires both time and food investment
        float food_needed = (total_duration_ms > 0) ? 0.0f : Cfg::SEED_TOTAL_FOOD;
        if (elapsed_ms >= seed_dur_ms && food_invested >= food_needed) {
            return BROOD_HATCH;
        }
        return BROOD_NONE;
    }

    return BROOD_NONE;
}
