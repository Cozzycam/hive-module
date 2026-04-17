/* Colony-wide state — one per colony. Ported from sim/colony.py. */
#pragma once
#include "config.h"

struct ColonyState {
    float    food_store          = 0.0f;
    float    food_total          = 0.0f;
    uint16_t population          = 0;
    uint16_t gatherer_count      = 0;
    uint16_t brood_egg           = 0;
    uint16_t brood_larva         = 0;
    uint16_t brood_pupa          = 0;
    uint16_t total_workers_born  = 0;
    float    peak_pressure       = 0.0f;
    int      recovery_boost_remaining = 0;

    float food_pressure() const;
    float target_gatherer_fraction() const;
    void  update_recovery_boost();
};
