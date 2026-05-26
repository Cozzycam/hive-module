/* Brood lifecycle: egg -> seed -> hatch. Real-time wall-clock. */
#pragma once
#include "config.h"

// Transition signals from Brood::tick()
enum BroodTransition : uint8_t {
    BROOD_NONE         = 0,
    BROOD_EGG_TO_SEED  = 1,
    BROOD_HATCH        = 3,
    BROOD_DIED         = 4,
};
// Legacy alias
constexpr BroodTransition BROOD_EGG_TO_LARVA  = BROOD_EGG_TO_SEED;
constexpr BroodTransition BROOD_LARVA_TO_PUPA = BROOD_HATCH;  // pupa stage gone

struct Brood {
    uint32_t   id = 0;         // persistent identity (shared allocator with Conker)
    uint32_t   tended_by = 0;  // first carer's ID (0 = untended)
    int8_t     x, y;
    BroodStage stage        = STAGE_EGG;
    Role       role         = ROLE_CONKER;
    uint32_t   stage_start_ms = 0;
    uint32_t   total_duration_ms = 0;  // founder bootstrap: total egg+seed time (0 = use defaults)
    float      hunger       = 0.0f;
    float      food_invested = 0.0f;

    void init(int8_t px, int8_t py, Role c = ROLE_CONKER);
    void init_with_duration(int8_t px, int8_t py, uint32_t duration_ms);
    BroodTransition tick(float dt);

    bool needs_feeding() const {
        return stage == STAGE_SEED && hunger > 0.3f;
    }

    void feed(float amount) {
        if (stage != STAGE_SEED) return;
        hunger = 0.0f;
        food_invested += amount;
    }

    bool alive() const { return stage != STAGE_DEAD; }
};
