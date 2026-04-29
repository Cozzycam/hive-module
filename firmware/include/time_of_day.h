/* Real-world time of day — RTC + NTP + solar position.
 *
 * Provides a global TimeOfDay struct that sim and renderer consume.
 * Sim reads phase/night_factor for idle bias; renderer reads
 * night_factor for palette shift. Nobody else writes to g_tod
 * except time_of_day_init/tick.
 */
#pragma once
#include <cstdint>

enum DayPhase : uint8_t {
    PHASE_NIGHT = 0,
    PHASE_DAWN  = 1,
    PHASE_DAY   = 2,
    PHASE_DUSK  = 3,
};

struct TimeOfDay {
    bool     rtc_valid      = false;
    bool     ntp_synced     = false;
    uint32_t unix_time      = 0;      // current UTC seconds since epoch
    int      local_hour     = 0;      // 0-23 local
    int      local_minute   = 0;      // 0-59
    int      day_of_year    = 1;      // 1-366
    int      year           = 2024;
    uint32_t sunrise_unix   = 0;      // today's sunrise, UTC unix
    uint32_t sunset_unix    = 0;      // today's sunset, UTC unix
    DayPhase phase          = PHASE_DAY;
    float    day_progress   = 0.5f;   // 0.0 at sunrise, 1.0 at sunset
    float    night_factor   = 0.0f;   // 0.0 = full day, 1.0 = full night
};

extern TimeOfDay g_tod;

// Call once in setup(), after Wire.begin(). Reads RTC, attempts NTP.
void time_of_day_init();

// Call once per second from main loop. Updates g_tod.
void time_of_day_tick();

// Recompute sunrise/sunset for current day. Called internally on
// day rollover and after NTP sync.
void time_of_day_recompute_sun();

// WiFi connect/disconnect helpers — reuse NTP credentials for OTA etc.
// connect returns true on success. disconnect turns WiFi off.
bool tod_wifi_connect(uint32_t timeout_ms = 10000);
void tod_wifi_disconnect();
