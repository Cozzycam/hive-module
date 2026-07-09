/* Host-build global definitions.
 *
 * On device these globals live in the network-coupled translation units
 * (time_of_day.cpp does NTP/RTC, weather.cpp hits Open-Meteo over WiFi). The
 * compiled sim + renderer only READ them, so instead of dragging networking
 * into the host build we define them here with fixed, benign values:
 *   - g_tod   : full daylight (night_factor 0) so the floor renders its day palette
 *   - g_weather: invalid → renderer draws no rain/snow/fog overlay (clear day)
 * A real host build would drive these from JS (device clock + a weather feed). */
#include "time_of_day.h"
#include "weather.h"
#include <Arduino.h>

HostSerial   Serial;
unsigned long g_host_millis = 0;

// Shared esp_random() state (see host/shims/esp_random.h). Reseeded per-install
// by host_seed() so every phone's colony is distinct. Same default as the old
// per-TU shim, so an un-seeded host build behaves as before.
uint32_t g_esp_rng_state = 0x9E3779B9u;

TimeOfDay g_tod;               // struct defaults: PHASE_DAY, night_factor 0.0
bool      g_warp_active = false;
Weather   g_weather;           // valid=false by default → no weather FX

// Colony-wide death tallies (defined in main.cpp on device); conker.cpp bumps them.
uint16_t g_deaths_old_age = 0;
uint16_t g_deaths_starved = 0;

// Handoff counters (main.cpp on device); coordinator bumps them. No peers here.
uint32_t g_handoffs_out = 0;
uint32_t g_handoffs_in = 0;
uint32_t g_handoffs_dropped = 0;

// WiFi-cred setters (time_of_day.cpp on device) — inert on host.
void tod_wifi_set_ssid(const char*) {}
void tod_wifi_set_pass(const char*) {}
