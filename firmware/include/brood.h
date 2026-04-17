/* Brood lifecycle: egg -> larva -> pupa -> hatch. Ported from sim/brood.py. */
#pragma once
#include "config.h"

// Transition signals from Brood::tick()
enum BroodTransition : uint8_t {
    BROOD_NONE         = 0,
    BROOD_EGG_TO_LARVA = 1,
    BROOD_LARVA_TO_PUPA = 2,
    BROOD_HATCH        = 3,
    BROOD_DIED         = 4,
};

struct Brood {
    int8_t     x, y;
    BroodStage stage        = STAGE_EGG;
    Role       role         = ROLE_MINOR;
    uint16_t   age          = 0;
    float      hunger       = 0.0f;
    float      fed_total    = 0.0f;
    int        larva_duration;
    float      food_needed;

    void init(int8_t px, int8_t py, Role c = ROLE_MINOR);

    // Returns transition signal (BROOD_HATCH, BROOD_EGG_TO_LARVA, etc.)
    BroodTransition tick();

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
