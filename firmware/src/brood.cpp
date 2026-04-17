#include "brood.h"

void Brood::init(int8_t px, int8_t py, Role c) {
    x = px;
    y = py;
    stage = STAGE_EGG;
    role = c;
    age = 0;
    hunger = 0.0f;
    fed_total = 0.0f;
    larva_duration = Cfg::ROLE_PARAMS[c].larva_duration;
    food_needed    = Cfg::ROLE_PARAMS[c].larva_food_needed;
}

BroodTransition Brood::tick() {
    if (stage == STAGE_DEAD) return BROOD_NONE;

    age++;

    if (stage == STAGE_EGG) {
        if (age >= Cfg::EGG_DURATION) {
            stage = STAGE_LARVA;
            age = 0;
            return BROOD_EGG_TO_LARVA;
        }
        return BROOD_NONE;
    }

    if (stage == STAGE_LARVA) {
        hunger += Cfg::LARVA_HUNGER_RATE;
        if (hunger >= Cfg::LARVA_STARVE) {
            stage = STAGE_DEAD;
            return BROOD_DIED;
        }
        if (age >= static_cast<uint16_t>(larva_duration)
                && fed_total >= food_needed) {
            stage = STAGE_PUPA;
            age = 0;
            return BROOD_LARVA_TO_PUPA;
        }
        return BROOD_NONE;
    }

    if (stage == STAGE_PUPA) {
        if (age >= Cfg::PUPA_DURATION)
            return BROOD_HATCH;
    }
    return BROOD_NONE;
}
