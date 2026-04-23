/* Brood lifecycle: egg -> larva -> pupa -> hatch. Real-time wall-clock. */
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
    uint32_t   stage_start_ms = 0;
    float      hunger       = 0.0f;
    float      food_invested = 0.0f;

    void init(int8_t px, int8_t py, Role c = ROLE_MINOR);
    BroodTransition tick(float dt);

    bool needs_feeding() const {
        return stage == STAGE_LARVA && hunger > 0.3f;
    }

    void feed(float amount) {
        if (stage != STAGE_LARVA) return;
        hunger = 0.0f;
        food_invested += amount;
    }

    bool alive() const { return stage != STAGE_DEAD; }
};
