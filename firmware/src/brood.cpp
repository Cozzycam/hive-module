#include "brood.h"

void Brood::init(int8_t px, int8_t py, Caste c) {
    x = px;
    y = py;
    stage = STAGE_EGG;
    caste = c;
    age = 0;
    hunger = 0.0f;
    fed_total = 0.0f;
    larva_duration = Cfg::CASTE_PARAMS[c].larva_duration;
    food_needed    = Cfg::CASTE_PARAMS[c].larva_food_needed;
}

bool Brood::tick() {
    if (stage == STAGE_DEAD) return false;

    age++;

    if (stage == STAGE_EGG) {
        if (age >= Cfg::EGG_DURATION) {
            stage = STAGE_LARVA;
            age = 0;
        }
        return false;
    }

    if (stage == STAGE_LARVA) {
        hunger += Cfg::LARVA_HUNGER_RATE;
        if (hunger >= Cfg::LARVA_STARVE) {
            stage = STAGE_DEAD;
            return false;
        }
        if (age >= static_cast<uint16_t>(larva_duration)
                && fed_total >= food_needed) {
            stage = STAGE_PUPA;
            age = 0;
        }
        return false;
    }

    if (stage == STAGE_PUPA) {
        if (age >= Cfg::PUPA_DURATION)
            return true;  // hatch signal
    }
    return false;
}
