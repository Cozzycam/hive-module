/* Weather fetch — Open-Meteo API, no key required. */
#include "weather.h"
#include "time_of_day.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

Weather g_weather;

// ---- Location (same as time_of_day.cpp) ----
static constexpr float LOC_LAT = 55.6117f;
static constexpr float LOC_LON = -4.6683f;

static const uint32_t WEATHER_SYNC_MS = 10UL * 60UL * 1000UL; // 10 minutes
static uint32_t _last_fetch_ms = 0;
static bool _first_fetch_done = false;

// ---- Classification ----

static WeatherCondition _classify_wmo(int code) {
    if (code <= 1)                          return WX_CLEAR;
    if (code == 2)                          return WX_PARTLY_CLOUDY;
    if (code == 3)                          return WX_OVERCAST;
    if (code == 45 || code == 48)           return WX_FOG;
    if (code >= 51 && code <= 57)           return WX_DRIZZLE;
    if ((code >= 61 && code <= 63) ||
        (code >= 66 && code <= 67) ||
        (code >= 80 && code <= 81))         return WX_RAIN;
    if (code == 65 || code == 82)           return WX_HEAVY_RAIN;
    if ((code >= 71 && code <= 77) ||
        (code >= 85 && code <= 86))         return WX_SNOW;
    if (code >= 95)                         return WX_THUNDERSTORM;
    return WX_CLEAR;
}

static TempSeverity _classify_temp(float c) {
    if (c < 0.0f)   return TEMP_FREEZING;
    if (c < 8.0f)   return TEMP_COLD;
    if (c < 18.0f)  return TEMP_MILD;
    if (c < 28.0f)  return TEMP_WARM;
    if (c < 35.0f)  return TEMP_HOT;
    return TEMP_EXTREME_HEAT;
}

static WindSeverity _classify_wind(float kmh) {
    if (kmh < 20.0f)  return WIND_CALM;
    if (kmh < 40.0f)  return WIND_BREEZY;
    if (kmh < 62.0f)  return WIND_WINDY;
    if (kmh < 90.0f)  return WIND_HIGH;
    return WIND_STORM;
}

// ---- Minimal JSON value extraction ----

static bool _json_float(const String& json, const char* key, float& out) {
    String needle = String("\"") + key + "\":";
    int idx = json.indexOf(needle);
    if (idx < 0) return false;
    idx += needle.length();
    while (idx < (int)json.length() && json[idx] == ' ') idx++;
    out = json.substring(idx).toFloat();
    return true;
}

static bool _json_int(const String& json, const char* key, int& out) {
    String needle = String("\"") + key + "\":";
    int idx = json.indexOf(needle);
    if (idx < 0) return false;
    idx += needle.length();
    while (idx < (int)json.length() && json[idx] == ' ') idx++;
    out = json.substring(idx).toInt();
    return true;
}

// ---- Fetch ----

bool weather_fetch() {
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,wind_speed_10m",
        LOC_LAT, LOC_LON);

    WiFiClientSecure client;
    client.setInsecure();  // skip cert verification (just weather data)

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[weather] HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    float temp, wind;
    int wmo;
    if (!_json_float(body, "temperature_2m", temp) ||
        !_json_int(body, "weather_code", wmo) ||
        !_json_float(body, "wind_speed_10m", wind)) {
        Serial.println("[weather] parse failed");
        return false;
    }

    g_weather.temperature_c  = temp;
    g_weather.wind_speed_kmh = wind;
    g_weather.wmo_code       = (uint8_t)wmo;
    g_weather.condition      = _classify_wmo(wmo);
    g_weather.temp           = _classify_temp(temp);
    g_weather.wind           = _classify_wind(wind);
    g_weather.valid          = true;

    static const char* wx_names[] = {
        "clear", "partly cloudy", "overcast", "fog",
        "drizzle", "rain", "heavy rain", "snow", "thunderstorm"
    };
    static const char* temp_names[] = {
        "freezing", "cold", "mild", "warm", "hot", "extreme heat"
    };
    static const char* wind_names[] = {
        "calm", "breezy", "windy", "high wind", "storm"
    };

    Serial.printf("[weather] %s, %.1fC (%s), wind %.0f km/h (%s), wmo=%d\n",
        wx_names[g_weather.condition],
        temp, temp_names[g_weather.temp],
        wind, wind_names[g_weather.wind], wmo);

    _last_fetch_ms = millis();
    return true;
}

// ---- Periodic tick ----

void weather_tick() {
    uint32_t now = millis();

    // First fetch: wait 30 seconds after boot to let ESP-NOW settle
    if (!_first_fetch_done) {
        if (now < 30000) return;
        _first_fetch_done = true;
        // Fall through to fetch
    } else {
        if (now - _last_fetch_ms < WEATHER_SYNC_MS) return;
    }

    Serial.println("[weather] syncing...");
    if (!tod_wifi_connect(10000)) return;
    weather_fetch();
    tod_wifi_disconnect();
}
