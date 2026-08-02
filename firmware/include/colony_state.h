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
    // Wall-clock unix of the last PAID interaction of each kind. On the colony
    // (not the conker) so it persists with the manifest — the PWA reloads
    // constantly, and a RAM-only cooldown would reset on every reload, which is
    // an exploit rather than a cooldown.
    uint32_t last_interact[3]    = {0, 0, 0};
    uint32_t owned_items         = 0;   // bitmask of shop items already bought. You pay ONCE:
                                        // re-selecting something you own is free, or buying a
                                        // second keepsake would silently cost you the first.
    uint16_t bugs                = 0;   // shop purse: critters caught, spendable on decor.
                                        // SEPARATE from Conker::catches, which is the
                                        // LIFETIME tally the Bug Hunter title reads — spending
                                        // must never be able to demote her.
    uint16_t total_workers_born  = 0;
    uint16_t worker_census       = 0;   // authoritative headcount (births - deaths)
    uint16_t pop_cap             = Cfg::POP_CAP_PER_MODULE;  // 10 × connected modules; hatch gate
    bool     eggs_dormant        = false;               // at cap with brood waiting (app notice)
    float    peak_pressure       = 0.0f;
    int      recovery_boost_remaining = 0;
    float    challenge_burn_mult = 1.0f;  // >1 during weather challenges (coordinator sets)

    float daily_burn() const;
    float food_pressure() const;
    float play_surplus() const;   // 0..1 — deep pantry frees time for play
    float target_gatherer_fraction() const;
    void  update_recovery_boost();
};
