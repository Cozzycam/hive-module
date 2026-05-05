/* VPS push — HTTPS POST colony snapshots + events to VPS. */
#include "vps_push.h"
#include "api_json.h"
#include "coordinator.h"
#include "time_of_day.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <mbedtls/md.h>

static constexpr uint32_t PUSH_INTERVAL_MS = 30000;  // 30s
static constexpr int MAX_SECRET_LEN = 64;
static constexpr int MAX_URL_LEN = 128;

static char _secret[MAX_SECRET_LEN] = {};
static char _endpoint[MAX_URL_LEN] = {};
static bool _configured = false;
static uint32_t _last_push_ms = 0;
static uint32_t _last_pushed_unix = 0;  // cursor: events after this have been pushed

// ---- HMAC-SHA256 ----

static void _hmac_sha256(const char* key, int key_len,
                          const char* data, int data_len,
                          char* out_hex, int out_hex_len) {
    uint8_t hash[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, key_len);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)data, data_len);
    mbedtls_md_hmac_finish(&ctx, hash);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32 && (i * 2 + 1) < out_hex_len; i++)
        snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
}

// ---- NVS persistence ----

static void _load_config() {
    Preferences prefs;
    prefs.begin("vps", true);
    prefs.getString("secret", _secret, MAX_SECRET_LEN);
    prefs.getString("endpoint", _endpoint, MAX_URL_LEN);
    _last_pushed_unix = prefs.getULong("cursor", 0);
    prefs.end();

    _configured = (_secret[0] != '\0' && _endpoint[0] != '\0');
    if (_configured)
        Serial.printf("[vps] configured — endpoint: %s\n", _endpoint);
    else
        Serial.println("[vps] not configured (use 'vps secret <key>' and 'vps endpoint <url>')");
}

static void _save_cursor() {
    Preferences prefs;
    prefs.begin("vps", false);
    prefs.putULong("cursor", _last_pushed_unix);
    prefs.end();
}

// ---- HTTP POST with HMAC ----

static bool _post(const char* path, const char* body, int body_len) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);

    // Compute HMAC
    char hmac[65] = {};
    _hmac_sha256(_secret, strlen(_secret), body, body_len, hmac, sizeof(hmac));

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-HMAC-SHA256", hmac);
    http.setTimeout(10000);

    int code = http.POST((uint8_t*)body, body_len);
    http.end();

    if (code >= 200 && code < 300) return true;
    if (code > 0)
        Serial.printf("[vps] POST %s — HTTP %d\n", path, code);
    else
        Serial.printf("[vps] POST %s — failed (err=%d)\n", path, code);
    return false;
}

// ---- Public API ----

void vps_push_init() {
    _load_config();
    _last_push_ms = millis();
}

void vps_push_tick(Coordinator& coord) {
    if (!_configured || !WiFi.isConnected()) return;

    uint32_t now = millis();
    if (now - _last_push_ms < PUSH_INTERVAL_MS) return;
    _last_push_ms = now;

    // Push colony snapshot
    char* buf = (char*)malloc(4096);
    if (!buf) return;

    size_t len = api_colony_json(coord, buf, 4096);
    if (len > 0) {
        char path[80];
        snprintf(path, sizeof(path), "/api/v1/colonies/%s/snapshot",
                 coord.registry.manifest().colony_id);
        _post(path, buf, len);
    }

    // Push events since last cursor
    if (_last_pushed_unix > 0 || g_tod.unix_time > 0) {
        struct EventCtx { String* str; bool has; };
        String events = "{\"events\":[";
        EventCtx ctx = {&events, false};

        coord.journal.read_day(g_tod.unix_time,
            [](const char* line, void* raw) -> bool {
                EventCtx* c = (EventCtx*)raw;
                if (c->str->length() > 16000) return false;
                if (c->has) *c->str += ",";
                *c->str += line;
                c->has = true;
                return true;
            }, &ctx);

        events += "]}";

        if (ctx.has) {
            char path[80];
            snprintf(path, sizeof(path), "/api/v1/colonies/%s/events",
                     coord.registry.manifest().colony_id);
            if (_post(path, events.c_str(), events.length())) {
                _last_pushed_unix = g_tod.unix_time;
                _save_cursor();
            }
        }
    }

    free(buf);
}

void vps_push_set_secret(const char* secret) {
    strlcpy(_secret, secret, MAX_SECRET_LEN);
    Preferences prefs;
    prefs.begin("vps", false);
    prefs.putString("secret", _secret);
    prefs.end();
    _configured = (_secret[0] != '\0' && _endpoint[0] != '\0');
    Serial.println("[vps] secret updated");
}

void vps_push_set_endpoint(const char* url) {
    strlcpy(_endpoint, url, MAX_URL_LEN);
    Preferences prefs;
    prefs.begin("vps", false);
    prefs.putString("endpoint", _endpoint);
    prefs.end();
    _configured = (_secret[0] != '\0' && _endpoint[0] != '\0');
    Serial.printf("[vps] endpoint set: %s\n", _endpoint);
}
