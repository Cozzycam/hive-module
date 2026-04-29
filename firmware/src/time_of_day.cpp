/* Real-world time of day — RTC, NTP, solar calculation, phase. */
#include "time_of_day.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include <cmath>

// ---- Location (Irvine, Scotland) ----
static constexpr float LOC_LAT = 55.6117f;
static constexpr float LOC_LON = -4.6683f;   // west negative (ISO)

// ---- Timing ----
static constexpr int CIVIL_TWILIGHT_MINUTES = 30;
static constexpr int NTP_RESYNC_HOURS       = 24;

// ---- WiFi credentials ----
// Set via environment or replace locally (do not commit real credentials)
static const char* WIFI_SSID = "YOUR_SSID";
static const char* WIFI_PASS = "YOUR_PASS";

// ---- PCF85063 RTC ----
static constexpr uint8_t RTC_ADDR = 0x51;

// ---- Global ----
TimeOfDay g_tod;

static uint32_t _last_ntp_sync_ms  = 0;
static int      _prev_local_day    = -1;
static bool     _simulated_clock   = false;
static uint32_t _sim_clock_base_ms = 0;
static uint32_t _sim_clock_epoch   = 0;

// ================================================================
//  BCD helpers
// ================================================================

static uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

// ================================================================
//  BST (British Summer Time) — last Sunday of March/October
// ================================================================

static int _last_sunday(int year, int month) {
    // Day of week for the last day of the month (Tomohiko Sakamoto)
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int days_in_month;
    if (month == 2)
        days_in_month = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 29 : 28;
    else if (month == 4 || month == 6 || month == 9 || month == 11)
        days_in_month = 30;
    else
        days_in_month = 31;

    int y = year - (month < 3 ? 1 : 0);
    int dow = (y + y/4 - y/100 + y/400 + t[month-1] + days_in_month) % 7;
    // dow: 0=Sunday ... 6=Saturday
    return days_in_month - dow;
}

static int _tz_offset_minutes(int year, int month, int day, int hour_utc) {
    // UK: UTC+0 in winter, UTC+1 (BST) from last Sunday of March 01:00 UTC
    //     to last Sunday of October 01:00 UTC
    int bst_start_day = _last_sunday(year, 3);
    int bst_end_day   = _last_sunday(year, 10);

    bool after_start = (month > 3) || (month == 3 && day > bst_start_day)
                    || (month == 3 && day == bst_start_day && hour_utc >= 1);
    bool before_end  = (month < 10) || (month == 10 && day < bst_end_day)
                    || (month == 10 && day == bst_end_day && hour_utc < 1);

    return (after_start && before_end) ? 60 : 0;
}

// ================================================================
//  Date helpers
// ================================================================

static bool _is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int _day_of_year(int year, int month, int day) {
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int doy = cum[month - 1] + day;
    if (month > 2 && _is_leap(year)) doy++;
    return doy;
}

static void _unix_to_ymd(uint32_t t, int& year, int& month, int& day,
                          int& hour, int& minute) {
    // Minimal unix to calendar conversion
    uint32_t s = t;
    minute = (s / 60) % 60;
    hour   = (s / 3600) % 24;
    uint32_t days = s / 86400;
    // Days since 1970-01-01
    int y = 1970;
    while (true) {
        int diy = _is_leap(y) ? 366 : 365;
        if (days < (uint32_t)diy) break;
        days -= diy;
        y++;
    }
    year = y;
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    while (m < 12) {
        int md = mdays[m];
        if (m == 1 && _is_leap(y)) md = 29;
        if (days < (uint32_t)md) break;
        days -= md;
        m++;
    }
    month = m + 1;
    day = days + 1;
}

static uint32_t _ymd_to_unix(int y, int m, int d, int h, int min) {
    // Inverse of above — not needed for arbitrary dates, just for
    // constructing sunrise/sunset unix times from today's date.
    uint32_t days = 0;
    for (int yr = 1970; yr < y; yr++)
        days += _is_leap(yr) ? 366 : 365;
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int mo = 0; mo < m - 1; mo++) {
        days += mdays[mo];
        if (mo == 1 && _is_leap(y)) days++;
    }
    days += d - 1;
    return days * 86400 + h * 3600 + min * 60;
}

// ================================================================
//  NOAA sunrise/sunset
// ================================================================

static constexpr float DEG2RAD = 0.0174532925f;
static constexpr float RAD2DEG = 57.2957795131f;

// Returns sunrise/sunset in UTC minutes from midnight.
// zenith: 90.833 for sunrise/sunset, 96.0 for civil twilight.
// Returns false if no rise/set (polar day/night).
static bool _sun_time(float lat, float lon, int doy, int days_in_year,
                      float zenith, float& out_rise, float& out_set) {
    float gamma = (2.0f * M_PI / days_in_year) * (doy - 1.5f);

    float eqtime = 229.18f * (0.000075f
        + 0.001868f * cosf(gamma)
        - 0.032077f * sinf(gamma)
        - 0.014615f * cosf(2.0f * gamma)
        - 0.040849f * sinf(2.0f * gamma));

    float decl = 0.006918f
        - 0.399912f * cosf(gamma)
        + 0.070257f * sinf(gamma)
        - 0.006758f * cosf(2.0f * gamma)
        + 0.000907f * sinf(2.0f * gamma)
        - 0.002697f * cosf(3.0f * gamma)
        + 0.001480f * sinf(3.0f * gamma);

    float cos_ha = (cosf(zenith * DEG2RAD) / (cosf(lat * DEG2RAD) * cosf(decl)))
                 - (tanf(lat * DEG2RAD) * tanf(decl));

    if (cos_ha > 1.0f)  return false;   // polar night
    if (cos_ha < -1.0f) return false;   // midnight sun

    float ha = acosf(cos_ha) * RAD2DEG;

    out_rise = 720.0f - 4.0f * (lon + ha) - eqtime;
    out_set  = 720.0f - 4.0f * (lon - ha) - eqtime;

    if (out_rise < 0)    out_rise += 1440.0f;
    if (out_rise > 1440) out_rise -= 1440.0f;
    if (out_set < 0)     out_set  += 1440.0f;
    if (out_set > 1440)  out_set  -= 1440.0f;

    return true;
}

void time_of_day_recompute_sun() {
    // Use local date — day rollover triggers on local time,
    // so sunrise/sunset must be computed for the local calendar day.
    int y, m, d, h, mi;
    _unix_to_ymd(g_tod.unix_time, y, m, d, h, mi);
    int tz = _tz_offset_minutes(y, m, d, h);
    uint32_t local = g_tod.unix_time + tz * 60;
    int ly, lm, ld, lh, lmi;
    _unix_to_ymd(local, ly, lm, ld, lh, lmi);

    int doy = _day_of_year(ly, lm, ld);
    int diy = _is_leap(ly) ? 366 : 365;
    float rise_min, set_min;

    if (_sun_time(LOC_LAT, LOC_LON, doy, diy,
                  90.833f, rise_min, set_min)) {
        // rise_min/set_min are UTC minutes from midnight —
        // anchor to UTC midnight of the local date
        uint32_t midnight = _ymd_to_unix(ly, lm, ld, 0, 0);
        g_tod.sunrise_unix = midnight + (uint32_t)(rise_min * 60.0f);
        g_tod.sunset_unix  = midnight + (uint32_t)(set_min * 60.0f);
    } else {
        // Polar edge case — use 6:00/18:00 UTC as fallback
        uint32_t midnight = _ymd_to_unix(ly, lm, ld, 0, 0);
        g_tod.sunrise_unix = midnight + 6 * 3600;
        g_tod.sunset_unix  = midnight + 18 * 3600;
    }

    Serial.printf("[tod] sun: rise=%02d:%02d  set=%02d:%02d UTC  (doy=%d)\n",
        (int)((g_tod.sunrise_unix % 86400) / 3600),
        (int)((g_tod.sunrise_unix % 3600) / 60),
        (int)((g_tod.sunset_unix % 86400) / 3600),
        (int)((g_tod.sunset_unix % 3600) / 60),
        g_tod.day_of_year);
}

// ================================================================
//  Phase + night_factor calculation
// ================================================================

static float _smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void _update_phase() {
    uint32_t t = g_tod.unix_time;
    uint32_t rise = g_tod.sunrise_unix;
    uint32_t set  = g_tod.sunset_unix;
    uint32_t tw   = CIVIL_TWILIGHT_MINUTES * 60;

    uint32_t dawn_start = rise - tw;
    uint32_t dawn_end   = rise + tw;
    uint32_t dusk_start = set - tw;
    uint32_t dusk_end   = set + tw;

    DayPhase prev_phase = g_tod.phase;

    if (t < dawn_start || t > dusk_end) {
        g_tod.phase = PHASE_NIGHT;
        g_tod.night_factor = 1.0f;
    } else if (t < dawn_end) {
        g_tod.phase = PHASE_DAWN;
        g_tod.night_factor = 1.0f - _smoothstep(
            (float)dawn_start, (float)dawn_end, (float)t);
    } else if (t <= dusk_start) {
        g_tod.phase = PHASE_DAY;
        g_tod.night_factor = 0.0f;
    } else {
        g_tod.phase = PHASE_DUSK;
        g_tod.night_factor = _smoothstep(
            (float)dusk_start, (float)dusk_end, (float)t);
    }

    // day_progress: 0 at sunrise, 1 at sunset
    if (t <= rise)      g_tod.day_progress = 0.0f;
    else if (t >= set)  g_tod.day_progress = 1.0f;
    else                g_tod.day_progress = (float)(t - rise) / (float)(set - rise);

    // Log phase changes
    if (g_tod.phase != prev_phase) {
        static const char* names[] = {"NIGHT", "DAWN", "DAY", "DUSK"};
        Serial.printf("[tod] phase -> %s  (night_factor=%.2f, local %02d:%02d)\n",
            names[g_tod.phase], g_tod.night_factor,
            g_tod.local_hour, g_tod.local_minute);
    }
}

// ================================================================
//  RTC (PCF85063)
// ================================================================

static bool _rtc_read(uint32_t& out_unix) {
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x04);  // seconds register
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom(RTC_ADDR, (uint8_t)7) < 7) return false;
    uint8_t raw[7];
    for (int i = 0; i < 7; i++) raw[i] = Wire.read();

    // Check oscillator stop flag (bit 7 of seconds)
    if (raw[0] & 0x80) return false;

    int sec   = bcd_to_dec(raw[0] & 0x7F);
    int min   = bcd_to_dec(raw[1] & 0x7F);
    int hour  = bcd_to_dec(raw[2] & 0x3F);
    int day   = bcd_to_dec(raw[3] & 0x3F);
    // raw[4] = weekday, skip
    int month = bcd_to_dec(raw[5] & 0x1F);
    int year  = 2000 + bcd_to_dec(raw[6]);

    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31)
        return false;

    out_unix = _ymd_to_unix(year, month, day, hour, min) + sec;
    return true;
}

static void _rtc_write(uint32_t unix_time) {
    int y, m, d, h, mi;
    _unix_to_ymd(unix_time, y, m, d, h, mi);
    int sec = unix_time % 60;

    // Compute weekday (Zeller-like, 0=Sunday)
    // Using Tomohiko Sakamoto
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = y - (m < 3 ? 1 : 0);
    int dow = (yy + yy/4 - yy/100 + yy/400 + t[m-1] + d) % 7;

    // Stop RTC
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x00);
    Wire.write(0x20);  // STOP bit
    Wire.endTransmission();

    // Write time registers 0x04-0x0A
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x04);
    Wire.write(dec_to_bcd(sec));          // seconds (clears OS flag)
    Wire.write(dec_to_bcd(mi));           // minutes
    Wire.write(dec_to_bcd(h));            // hours
    Wire.write(dec_to_bcd(d));            // day
    Wire.write(dec_to_bcd(dow));          // weekday
    Wire.write(dec_to_bcd(m));            // month
    Wire.write(dec_to_bcd(y - 2000));     // year
    Wire.endTransmission();

    // Restart RTC
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x00);
    Wire.write(0x00);  // clear STOP
    Wire.endTransmission();

    Serial.printf("[tod] RTC set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
        y, m, d, h, mi, sec);
}

// ================================================================
//  NTP
// ================================================================

// Disconnect from AP but keep WiFi STA alive for ESP-NOW.
// Re-pins radio to channel 1 so topology resumes immediately.
static void _wifi_teardown() {
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

static bool _ntp_sync() {
    Serial.printf("[tod] WiFi connecting to '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 10000) {
            Serial.println("[tod] WiFi timeout");
            _wifi_teardown();
            return false;
        }
        delay(100);
    }
    Serial.printf("[tod] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());

    // NTP sync via ESP32 SNTP
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    struct tm tm;
    start = millis();
    while (!getLocalTime(&tm, 100)) {
        if (millis() - start > 10000) {
            Serial.println("[tod] NTP timeout");
            _wifi_teardown();
            return false;
        }
        delay(100);
    }

    time_t now;
    time(&now);
    g_tod.unix_time = (uint32_t)now;
    g_tod.ntp_synced = true;
    _last_ntp_sync_ms = millis();

    Serial.printf("[tod] NTP sync OK: unix=%lu\n", g_tod.unix_time);

    // Write to RTC for offline persistence
    _rtc_write(g_tod.unix_time);

    _wifi_teardown();
    return true;
}

// ================================================================
//  Simulated clock fallback
// ================================================================

static void _start_simulated_clock() {
    _simulated_clock = true;
    _sim_clock_base_ms = millis();
    // Start at midnight UTC
    // Use a plausible date for solar calc
    _sim_clock_epoch = _ymd_to_unix(2026, 4, 20, 0, 0);
    g_tod.unix_time = _sim_clock_epoch;
    Serial.println("[tod] No RTC or NTP — simulated 24h clock");
}

// ================================================================
//  WiFi helpers (shared with OTA)
// ================================================================

bool tod_wifi_connect(uint32_t timeout_ms) {
    Serial.printf("[wifi] Connecting to '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            Serial.println("[wifi] Connect timeout");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
        delay(100);
    }
    Serial.printf("[wifi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
}

void tod_wifi_disconnect() {
    _wifi_teardown();
}

// ================================================================
//  Public API
// ================================================================

void time_of_day_init() {
    // 1. Try RTC
    uint32_t rtc_time;
    if (_rtc_read(rtc_time)) {
        g_tod.unix_time = rtc_time;
        g_tod.rtc_valid = true;
        Serial.printf("[tod] RTC valid: unix=%lu\n", rtc_time);
    } else {
        Serial.println("[tod] RTC invalid or absent");
    }

    // 2. Try NTP (non-blocking-ish, 10s timeout)
    if (!_ntp_sync() && !g_tod.rtc_valid) {
        // 3. Fallback: simulated clock
        _start_simulated_clock();
    }

    // Populate calendar fields from unix_time
    int y, m, d, h, mi;
    _unix_to_ymd(g_tod.unix_time, y, m, d, h, mi);
    g_tod.year = y;

    int tz = _tz_offset_minutes(y, m, d, h);
    uint32_t local = g_tod.unix_time + tz * 60;
    int ly, lm, ld, lh, lmi;
    _unix_to_ymd(local, ly, lm, ld, lh, lmi);
    g_tod.local_hour   = lh;
    g_tod.local_minute = lmi;
    g_tod.day_of_year  = _day_of_year(ly, lm, ld);
    _prev_local_day    = ld;

    time_of_day_recompute_sun();
    _update_phase();

    Serial.printf("[tod] init: local %02d:%02d  phase=%d  nf=%.2f\n",
        g_tod.local_hour, g_tod.local_minute,
        g_tod.phase, g_tod.night_factor);
}

void time_of_day_tick() {
    // Advance clock
    if (_simulated_clock) {
        uint32_t elapsed_ms = millis() - _sim_clock_base_ms;
        g_tod.unix_time = _sim_clock_epoch + elapsed_ms / 1000;
    } else if (g_tod.ntp_synced) {
        // Re-read system time (kept in sync by SNTP) instead of
        // incrementing — avoids drift from missed ticks.
        time_t now;
        time(&now);
        g_tod.unix_time = (uint32_t)now;
    } else {
        g_tod.unix_time++;
    }

    // Compute local time with BST
    int y, m, d, h, mi;
    _unix_to_ymd(g_tod.unix_time, y, m, d, h, mi);
    g_tod.year = y;

    int tz = _tz_offset_minutes(y, m, d, h);
    uint32_t local = g_tod.unix_time + tz * 60;
    int ly, lm, ld, lh, lmi;
    _unix_to_ymd(local, ly, lm, ld, lh, lmi);
    g_tod.local_hour   = lh;
    g_tod.local_minute = lmi;
    g_tod.day_of_year  = _day_of_year(ly, lm, ld);

    // Day rollover — recompute sun
    if (ld != _prev_local_day) {
        _prev_local_day = ld;
        time_of_day_recompute_sun();
        Serial.printf("[tod] day rollover -> %04d-%02d-%02d\n", ly, lm, ld);
    }

    // NTP resync
    if (!_simulated_clock && millis() - _last_ntp_sync_ms > (uint32_t)NTP_RESYNC_HOURS * 3600000UL) {
        _ntp_sync();
    }

    _update_phase();
}
