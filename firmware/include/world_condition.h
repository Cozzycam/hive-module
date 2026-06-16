/* WorldCondition — unified environment state for sim subsystems.
 *
 * Updated once per tick. Subsystems read from here for time, weather,
 * season, and active challenges. Phase 5 stubs weather/season fields;
 * future phases wire real data.
 */
#pragma once
#include "time_of_day.h"
#include <cstdint>

// ---- Challenge types ----
enum ChallengeType : uint8_t {
    CHALLENGE_NONE     = 0,
    CHALLENGE_HEATWAVE = 1,
    CHALLENGE_COLD_SNAP = 2,
    CHALLENGE_DROUGHT  = 3,
    CHALLENGE_STORM    = 4,
    CHALLENGE_COUNT
};

// ---- Season (stub) ----
enum Season : uint8_t {
    SEASON_INDETERMINATE = 0,
    SEASON_SPRING = 1,
    SEASON_SUMMER = 2,
    SEASON_AUTUMN = 3,
    SEASON_WINTER = 4,
};

// ---- Trait bits (sim-resolved) ----
enum TraitBit : uint32_t {
    TRAIT_PIONEER            = (1 << 0),
    TRAIT_ELDER              = (1 << 1),
    TRAIT_BONDED             = (1 << 2),
    TRAIT_SURVIVED_HEATWAVE  = (1 << 3),
    TRAIT_SURVIVED_COLD_SNAP = (1 << 4),
    TRAIT_SURVIVED_DROUGHT   = (1 << 5),
    TRAIT_SURVIVED_STORM     = (1 << 6),
    TRAIT_CATCHER            = (1 << 7),   // caught a visiting critter ("Bug Hunter")
    // Bits 8-31 reserved for future traits
};

// Map challenge type to survival trait bit
inline uint32_t challenge_survival_trait(uint8_t type) {
    switch (type) {
        case CHALLENGE_HEATWAVE:  return TRAIT_SURVIVED_HEATWAVE;
        case CHALLENGE_COLD_SNAP: return TRAIT_SURVIVED_COLD_SNAP;
        case CHALLENGE_DROUGHT:   return TRAIT_SURVIVED_DROUGHT;
        case CHALLENGE_STORM:     return TRAIT_SURVIVED_STORM;
        default: return 0;
    }
}

// ---- Active challenge instance ----
struct ActiveChallenge {
    uint8_t  type = CHALLENGE_NONE;
    float    severity = 0;
    uint32_t start_tick = 0;
    uint32_t start_unix = 0;
};

// ---- WorldCondition ----
struct WorldCondition {
    // Time (synced from g_tod each tick)
    TimeOfDay tod;
    uint16_t  day_of_year = 1;

    // Weather stubs (Phase 5+N)
    float    temperature_c = 20.0f;
    float    humidity_pct  = 50.0f;
    uint8_t  weather       = 0;  // WEATHER_CLEAR
    Season   season        = SEASON_INDETERMINATE;

    // Active challenges
    static constexpr int MAX_CHALLENGES = 4;
    ActiveChallenge active[MAX_CHALLENGES];
    int active_count = 0;

    // Start a challenge. Returns index, or -1 if full.
    int start_challenge(uint8_t type, float severity, uint32_t tick, uint32_t unix_time) {
        if (active_count >= MAX_CHALLENGES) return -1;
        active[active_count] = {type, severity, tick, unix_time};
        return active_count++;
    }

    // End challenge at index. Returns the ended challenge.
    ActiveChallenge end_challenge(int idx) {
        ActiveChallenge ended = {};
        if (idx < 0 || idx >= active_count) return ended;
        ended = active[idx];
        active[idx] = active[active_count - 1];
        active_count--;
        return ended;
    }

    bool has_active_challenge() const { return active_count > 0; }
};
