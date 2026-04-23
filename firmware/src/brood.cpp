#include "brood.h"
#include <Arduino.h>

void Brood::init(int8_t px, int8_t py, Role c) {
    x = px;
    y = py;
    stage = STAGE_EGG;
    role = c;
    stage_start_ms = millis();
    hunger = 0.0f;
    food_invested = 0.0f;
}

BroodTransition Brood::tick(float dt) {
    if (stage == STAGE_DEAD) return BROOD_NONE;

    uint32_t elapsed_ms = millis() - stage_start_ms;
    float elapsed_days = elapsed_ms / (Cfg::SECS_PER_DAY * 1000.0f);

    if (stage == STAGE_EGG) {
        if (elapsed_days >= Cfg::EGG_DURATION_DAYS) {
            stage = STAGE_LARVA;
            stage_start_ms = millis();
            hunger = 0.0f;
            return BROOD_EGG_TO_LARVA;
        }
        return BROOD_NONE;
    }

    if (stage == STAGE_LARVA) {
        // Hunger accumulates in real-time (normalized 0-1 over ~1 day without feeding)
        hunger += dt / Cfg::SECS_PER_DAY;
        if (hunger >= 1.0f) {
            stage = STAGE_DEAD;
            return BROOD_DIED;
        }
        if (elapsed_days >= Cfg::LARVA_DURATION_DAYS
                && food_invested >= Cfg::LARVA_TOTAL_FOOD) {
            stage = STAGE_PUPA;
            stage_start_ms = millis();
            return BROOD_LARVA_TO_PUPA;
        }
        return BROOD_NONE;
    }

    if (stage == STAGE_PUPA) {
        if (elapsed_days >= Cfg::PUPA_DURATION_DAYS)
            return BROOD_HATCH;
    }
    return BROOD_NONE;
}
