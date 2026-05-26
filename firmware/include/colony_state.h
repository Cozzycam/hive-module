/* Colony-wide state — one per colony. Ported from sim/colony.py. */
#pragma once
#include "config.h"

struct ColonyState {
    float    food_store          = 0.0f;
    float    food_total          = 0.0f;
    uint16_t population          = 0;
    uint16_t gatherer_count      = 0;
    uint16_t brood_egg           = 0;
    uint16_t brood_seed          = 0;
    uint16_t brood_larva         = 0;   // legacy alias, kept for API (same as brood_seed)
    uint16_t brood_pupa          = 0;   // always 0 (pupa stage removed)
    uint16_t total_workers_born  = 0;
    uint16_t worker_census       = 0;   // authoritative headcount (births - deaths)
    float    peak_pressure       = 0.0f;
    int      recovery_boost_remaining = 0;

    float daily_burn() const;
    float food_pressure() const;
    float target_gatherer_fraction() const;
    void  update_recovery_boost();
};
