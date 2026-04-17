/* Brood lifecycle: egg → larva → pupa → hatch. Ported from sim/brood.py. */
#pragma once
#include "config.h"

struct Brood {
    int8_t     x, y;
    BroodStage stage        = STAGE_EGG;
    Caste      caste        = CASTE_MINOR;
    uint16_t   age          = 0;
    float      hunger       = 0.0f;
    float      fed_total    = 0.0f;
    int        larva_duration;
    float      food_needed;

    void init(int8_t px, int8_t py, Caste c = CASTE_MINOR);

    // Returns true if the pupa is ready to hatch.
    bool tick();

    bool needs_feeding() const {
        return stage == STAGE_LARVA && hunger > 0.5f;
    }

    void feed(float amount) {
        if (stage != STAGE_LARVA) return;
        hunger = 0.0f;
        fed_total += amount;
    }

    bool alive() const { return stage != STAGE_DEAD; }
};
