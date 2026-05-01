/* Local weather state — fetched from Open-Meteo API.
 * Categorizes raw data into severity enums for sim use.
 * Manages its own WiFi connections (brief, ~5s disruption to ESP-NOW). */
#pragma once
#include <cstdint>

enum WeatherCondition : uint8_t {
    WX_CLEAR = 0,
    WX_PARTLY_CLOUDY,
    WX_OVERCAST,
    WX_FOG,
    WX_DRIZZLE,
    WX_RAIN,
    WX_HEAVY_RAIN,
    WX_SNOW,
    WX_THUNDERSTORM,
};

enum TempSeverity : uint8_t {
    TEMP_FREEZING = 0,   // < 0 C
    TEMP_COLD,           // 0–8 C
    TEMP_MILD,           // 8–18 C
    TEMP_WARM,           // 18–28 C
    TEMP_HOT,            // 28–35 C
    TEMP_EXTREME_HEAT,   // > 35 C
};

enum WindSeverity : uint8_t {
    WIND_CALM = 0,       // < 20 km/h
    WIND_BREEZY,         // 20–40 km/h
    WIND_WINDY,          // 40–62 km/h
    WIND_HIGH,           // 62–90 km/h  (amber warning territory)
    WIND_STORM,          // > 90 km/h   (red warning territory)
};

struct Weather {
    WeatherCondition condition = WX_CLEAR;
    TempSeverity     temp      = TEMP_MILD;
    WindSeverity     wind      = WIND_CALM;
    float temperature_c  = 15.0f;   // raw for debug/logging
    float wind_speed_kmh = 0.0f;
    uint8_t wmo_code     = 0;
    bool  valid          = false;
};

extern Weather g_weather;

// Fetch weather now (WiFi must already be connected).
bool weather_fetch();

// Call from main loop — manages timing + WiFi for periodic re-fetch.
void weather_tick();
