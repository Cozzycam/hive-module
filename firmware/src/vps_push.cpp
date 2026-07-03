/* VPS push — HTTPS POST colony snapshots + events to VPS. */
#include "vps_push.h"
#include "api_json.h"
#include "coordinator.h"
#include "time_of_day.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <Update.h>

static constexpr uint32_t PUSH_INTERVAL_MS = 30000;  // 30s
static constexpr int MAX_SECRET_LEN = 80;
static constexpr int MAX_URL_LEN = 128;

// Default endpoint — not a secret; any module enrolls itself on first push
static const char* DEFAULT_ENDPOINT = "http://hive.campbell.fish";

static char _secret[MAX_SECRET_LEN] = {};
static char _endpoint[MAX_URL_LEN] = {};
static bool _configured = false;
static bool _enrolled = false;  // VPS has adopted our secret (TOFU done)
static uint32_t _last_push_ms = 0;
static uint32_t _last_pushed_unix = 0;  // cursor: events after this have been pushed
static uint32_t _push_ok_count = 0;
static uint32_t _push_fail_count = 0;

// Journal line cursor — how many lines of the current day's journal have
// been pushed. Before this existed, every cycle re-sent the day's file from
// line 0 into a 16KB String cap: once a busy day's journal outgrew 16KB the
// reader aborted before reaching anything new and the diary went silent
// until midnight (Amber's "no diary entries since 04:51" bug). Now each
// cycle skips what's already pushed and sends up to ~16KB of new lines.
static uint32_t _pushed_day = 0;    // unix day (unix/86400) the counter refers to
static uint32_t _pushed_lines = 0;  // journal lines of that day already pushed

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
    _pushed_day = prefs.getULong("jday", 0);
    _pushed_lines = prefs.getULong("jlines", 0);
    _enrolled = prefs.getBool("enrolled", false);
    prefs.end();

    // Self-provision: a fresh module mints its own secret and uses the
    // default endpoint — the VPS adopts the secret on first contact (TOFU).
    // Survives factory reset ("vps" namespace is deliberately kept).
    bool changed = false;
    if (_secret[0] == '\0') {
        for (int i = 0; i < 16; i++) {
            uint32_t r = esp_random();
            snprintf(_secret + i * 4, 5, "%04x", (unsigned)(r & 0xFFFF));
        }
        _enrolled = false;
        changed = true;
        Serial.println("[vps] minted device secret (will enroll on first push)");
    }
    if (_endpoint[0] == '\0') {
        strlcpy(_endpoint, DEFAULT_ENDPOINT, MAX_URL_LEN);
        changed = true;
    }
    if (changed) {
        Preferences wp;
        wp.begin("vps", false);
        wp.putString("secret", _secret);
        wp.putString("endpoint", _endpoint);
        wp.putBool("enrolled", _enrolled);
        wp.end();
    }

    _configured = true;
    Serial.printf("[vps] configured — endpoint: %s%s\r\n", _endpoint,
                  _enrolled ? "" : " (not yet enrolled)");
}

static void _save_cursor() {
    Preferences prefs;
    prefs.begin("vps", false);
    prefs.putULong("cursor", _last_pushed_unix);
    prefs.putULong("jday", _pushed_day);
    prefs.putULong("jlines", _pushed_lines);
    prefs.end();
}

// ---- HTTP POST with HMAC ----

static bool _post(const char* path, const char* body, int body_len) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);

    // Compute HMAC
    char hmac[65] = {};
    _hmac_sha256(_secret, strlen(_secret), body, body_len, hmac, sizeof(hmac));

    WiFiClient client;
    client.setTimeout(10);  // 10s socket timeout

    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-HMAC-SHA256", hmac);
    // Until the VPS has adopted our secret, offer it for first-contact
    // enrollment. Dropped from all requests once enrolled.
    if (!_enrolled) http.addHeader("X-Enroll-Secret", _secret);
    http.setTimeout(10000);

    int code = http.POST((uint8_t*)body, body_len);
    http.end();

    if (code >= 200 && code < 300) {
        _push_ok_count++;
        if (!_enrolled) {
            _enrolled = true;
            Preferences prefs;
            prefs.begin("vps", false);
            prefs.putBool("enrolled", true);
            prefs.end();
            Serial.println("[vps] enrolled with VPS");
        }
        return true;
    }
    _push_fail_count++;
    if (code > 0)
        Serial.printf("[vps] POST %s — HTTP %d\r\n", path, code);
    else
        Serial.printf("[vps] POST %s — failed (err=%d)\r\n", path, code);
    return false;
}

// ---- HTTP GET -> JSON ----

static bool _get_json(const char* path, JsonDocument& doc) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);

    WiFiClient client;
    client.setTimeout(10);

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);

    int code = http.GET();
    if (code < 200 || code >= 300) {
        http.end();
        return false;
    }
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    return !err;
}

// ---- Firmware OTA from VPS ----
// Authenticity is anchored in the manifest signature, not the transport: the
// VPS signs {version,url,md5} with this colony's HMAC secret, we verify it with
// our own secret, then verify the downloaded image against the (now-trusted)
// MD5. A forged trigger can at worst cause a legitimate update — never a
// malicious flash. Dual-bank means an interrupted download is harmless.
static bool _ota_from_vps(const char* colony_id) {
    if (_secret[0] == '\0' || WiFi.status() != WL_CONNECTED) {
        Serial.println("[ota] not ready (no secret / no WiFi)");
        return false;
    }

    // 1. Fetch the signed manifest
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/firmware", colony_id);
    JsonDocument doc;
    if (!_get_json(path, doc)) { Serial.println("[ota] manifest fetch failed"); return false; }

    uint32_t version = doc["version"] | 0;
    const char* url = doc["url"] | "";
    const char* md5 = doc["md5"] | "";
    const char* sig = doc["sig"] | "";
    if (version == 0 || !url[0] || !md5[0] || !sig[0]) {
        Serial.println("[ota] manifest incomplete");
        return false;
    }

    // 2. Verify the manifest signature with our device secret
    char signed_data[256];
    int n = snprintf(signed_data, sizeof(signed_data), "%lu\n%s\n%s",
                     (unsigned long)version, url, md5);
    char expect[65];
    _hmac_sha256(_secret, strlen(_secret), signed_data, n, expect, sizeof(expect));
    if (strncmp(expect, sig, 64) != 0) {
        Serial.println("[ota] SIGNATURE MISMATCH — refusing update");
        return false;
    }

    // 3. Only update if strictly newer
    if (version <= FW_VERSION) {
        Serial.printf("[ota] already current (v%lu, manifest v%lu)\r\n",
                      (unsigned long)FW_VERSION, (unsigned long)version);
        return false;
    }
    Serial.printf("[ota] verified manifest — updating v%lu -> v%lu\r\n",
                  (unsigned long)FW_VERSION, (unsigned long)version);

    // 4. Download + flash, verifying the authenticated MD5
    WiFiClient client;
    client.setTimeout(20);
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(20000);
    int code = http.GET();
    if (code != 200) { Serial.printf("[ota] download HTTP %d\r\n", code); http.end(); return false; }
    int len = http.getSize();
    if (len <= 0) { Serial.println("[ota] bad content length"); http.end(); return false; }

    if (!Update.begin(len)) {
        Serial.printf("[ota] Update.begin failed: %s\r\n", Update.errorString());
        http.end();
        return false;
    }
    Update.setMD5(md5);  // Update.end() fails if the image MD5 doesn't match
    size_t written = Update.writeStream(*http.getStreamPtr());
    http.end();
    if (written != (size_t)len) {
        Serial.printf("[ota] short write %u/%d\r\n", (unsigned)written, len);
        Update.abort();
        return false;
    }
    if (!Update.end(true)) {
        Serial.printf("[ota] verify/commit failed: %s\r\n", Update.errorString());
        return false;
    }

    // 5. Arm the rollback trial + satellite cascade, then reboot into the new image
    Serial.println("[ota] flash OK, MD5 verified — rebooting into new image");
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putBool("trial", true);
    prefs.putInt("boot_try", 0);
    prefs.putBool("cascade", true);
    prefs.end();
    return true;
}

void vps_ota_update(Coordinator& coord) {
    if (_ota_from_vps(coord.registry.manifest().colony_id)) {
        delay(200);
        ESP.restart();
    }
}

// ---- Command queue (app -> queen via VPS) ----

// reset_to_satellite is deferred until after the ack POST: once this board
// wipes and reboots as a satellite it never polls this colony's queue
// again, so an unacked command would clog it forever.
static bool    _pending_convert = false;
static uint8_t _pending_convert_role = 0;

static void _poll_commands(Coordinator& coord) {
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/commands/pending",
             coord.registry.manifest().colony_id);

    JsonDocument doc;
    if (!_get_json(path, doc)) return;

    JsonArray results = doc["results"];
    if (results.isNull() || results.size() == 0) return;

    String acks = "{\"ids\":[";
    bool first = true;
    for (JsonObject cmd : results) {
        const char* type = cmd["type"] | "";
        long id = cmd["id"] | 0L;
        if (id == 0) continue;

        if (strcmp(type, "name_conker") == 0) {
            uint32_t cid = cmd["payload"]["id"] | 0;
            const char* name = cmd["payload"]["name"] | "";
            coord.cmd_rename_conker(cid, name);
        } else if (strcmp(type, "feed_colony") == 0) {
            float amount = cmd["payload"]["amount"] | 0.0f;
            coord.cmd_feed_colony(amount);
        } else if (strcmp(type, "set_module_role") == 0) {
            const char* mod  = cmd["payload"]["module"] | "";
            const char* role = cmd["payload"]["role"] | "";
            uint16_t target = (uint16_t)strtol(mod, nullptr, 16);
            int r = module_role_from_str(role);
            if (target != 0 && r >= 0)
                coord.cmd_set_module_role(target, (uint8_t)r);
        } else if (strcmp(type, "set_floor_tint") == 0) {
            const char* mod   = cmd["payload"]["module"] | "";
            const char* color = cmd["payload"]["color"] | "";  // "#rrggbb" or "" = reset
            uint16_t target = (uint16_t)strtol(mod, nullptr, 16);
            uint32_t rgb = 0;
            if (color[0] == '#') rgb = (uint32_t)strtol(color + 1, nullptr, 16);
            if (target != 0)
                coord.cmd_set_floor_tint(target, (rgb >> 16) & 0xFF,
                                         (rgb >> 8) & 0xFF, rgb & 0xFF);
        } else if (strcmp(type, "gift_care_package") == 0) {
            const char* mod = cmd["payload"]["module"] | "";
            uint16_t target = (uint16_t)strtol(mod, nullptr, 16);  // 0 = any neighbour
            coord.cmd_gift_care_package(target);
        } else if (strcmp(type, "ota_update") == 0) {
            // Reboots into the new image on success. The command stays pending
            // until then; after the reboot the manifest reads "already current"
            // so it acks without re-flashing — no loop.
            vps_ota_update(coord);
        } else if (strcmp(type, "grant_wish") == 0) {
            uint32_t wid = cmd["payload"]["id"] | 0;
            coord.cmd_grant_wish(wid);
        } else if (strcmp(type, "set_followed") == 0) {
            // App pins — star these conkers on the glass
            uint32_t ids[Chamber::MAX_FOLLOWED];
            int n = 0;
            for (JsonVariant v : cmd["payload"]["ids"].as<JsonArray>()) {
                if (n >= Chamber::MAX_FOLLOWED) break;
                uint32_t id = v | 0;
                if (id != 0) ids[n++] = id;
            }
            coord.cmd_set_followed(ids, n);
        } else if (strcmp(type, "reset_to_satellite") == 0) {
            // Conversion: this board abandons its sovereign colony and
            // reboots as a blank satellite (or specialised role), ready to
            // join whichever queen it's pogo-connected to.
            const char* role = cmd["payload"]["role"] | "satellite";
            int r = module_role_from_str(role);
            if (r >= MODULE_SATELLITE) {   // never convert to queen this way
                _pending_convert = true;
                _pending_convert_role = (uint8_t)r;
            }
        }
        // Always ack — invalid commands must not clog the queue
        if (!first) acks += ",";
        acks += String(id);
        first = false;
    }
    acks += "]}";

    if (!first) {
        snprintf(path, sizeof(path), "/api/v1/colonies/%s/commands/ack",
                 coord.registry.manifest().colony_id);
        bool acked = _post(path, acks.c_str(), acks.length());

        // Deferred conversion — only after the ack landed
        if (_pending_convert && acked) {
            Serial.printf("[vps] converting to %s — wiping colony, rebooting\r\n",
                          module_role_str(_pending_convert_role));
            colony_reset_wipe();
            coord.set_role_nvs((ModuleRole)_pending_convert_role);
            delay(200);
            ESP.restart();
        }
        _pending_convert = false;
    }
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

    // Push colony snapshot (buffer sized for roster: ~100 bytes per conker)
    size_t buf_size = 4096 + coord.registry.living_count() * 512;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return;

    size_t len = api_colony_json(coord, buf, buf_size);
    if (len > 0) {
        char path[80];
        snprintf(path, sizeof(path), "/api/v1/colonies/%s/snapshot",
                 coord.registry.manifest().colony_id);
        _post(path, buf, len);
    }

    // Push events since the line cursor (16KB of new lines per cycle; a
    // backlog drains across cycles instead of silently truncating). The
    // cursor day only rolls forward once its file is FULLY drained, so a
    // day spent offline catches up from the 32GB SD archive when WiFi
    // returns — history is never dropped at midnight.
    if (g_tod.unix_time > 0) {
        uint32_t today = g_tod.unix_time / 86400;
        if (_pushed_day == 0 || _pushed_day > today) {
            _pushed_day = today;    // first run / clock weirdness
            _pushed_lines = 0;
        }

        struct EventCtx {
            String* str;
            bool has;
            bool capped;        // hit the batch cap — more remains in the file
            uint32_t skip;      // lines already pushed
            uint32_t seen;      // lines encountered this read
            uint32_t appended;  // new lines added to this batch
        };
        String events = "{\"events\":[";
        EventCtx ctx = {&events, false, false, _pushed_lines, 0, 0};

        coord.journal.read_day(_pushed_day * 86400 + 43200,  // noon of cursor day
            [](const char* line, void* raw) -> bool {
                EventCtx* c = (EventCtx*)raw;
                c->seen++;
                if (c->seen <= c->skip) return true;   // already pushed
                if (c->str->length() > 16000) { c->capped = true; return false; }
                if (c->has) *c->str += ",";
                *c->str += line;
                c->has = true;
                c->appended++;
                return true;
            }, &ctx);

        events += "]}";

        if (ctx.seen < ctx.skip) {
            // File has fewer lines than the cursor (SD swapped/reset) —
            // resync to what exists and let the next cycle push cleanly.
            // VPS-side dedup makes any overlap harmless.
            _pushed_lines = ctx.seen;
            _save_cursor();
        } else {
            bool sent_ok = true;
            if (ctx.has) {
                char path[80];
                snprintf(path, sizeof(path), "/api/v1/colonies/%s/events",
                         coord.registry.manifest().colony_id);
                sent_ok = _post(path, events.c_str(), events.length());
                if (sent_ok) {
                    _last_pushed_unix = g_tod.unix_time;
                    _pushed_lines += ctx.appended;
                }
            }
            // Cursor day fully drained and it's a PAST day — advance one
            // day per cycle until we reach today (empty days skip through)
            bool advanced = false;
            if (sent_ok && !ctx.capped && _pushed_day < today) {
                _pushed_day++;
                _pushed_lines = 0;
                advanced = true;
                Serial.printf("[vps] journal catch-up: advancing to day %lu\r\n",
                              (unsigned long)_pushed_day);
            }
            if (advanced || (ctx.has && sent_ok)) _save_cursor();
        }
    }

    free(buf);

    // Poll + apply queued app commands (rename, care packages, ...)
    _poll_commands(coord);
}

void vps_push_set_secret(const char* secret) {
    strlcpy(_secret, secret, MAX_SECRET_LEN);
    _enrolled = false;  // re-offer the new secret on next push
    Preferences prefs;
    prefs.begin("vps", false);
    prefs.putString("secret", _secret);
    prefs.putBool("enrolled", false);
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
    Serial.printf("[vps] endpoint set: %s\r\n", _endpoint);
}

void vps_push_status() {
    Serial.println("[vps] --- status ---");
    Serial.printf("  configured: %s\r\n", _configured ? "yes" : "no");
    Serial.printf("  endpoint:   %s\r\n", _endpoint[0] ? _endpoint : "(not set)");
    Serial.printf("  secret:     %s\r\n", _secret[0] ? "(set)" : "(not set)");
    Serial.printf("  enrolled:   %s\r\n", _enrolled ? "yes" : "no");
    Serial.printf("  wifi:       %s\r\n", WiFi.isConnected() ? "connected" : "disconnected");
    uint32_t ago = (millis() - _last_push_ms) / 1000;
    Serial.printf("  last push:  %lus ago\r\n", ago);
    Serial.printf("  cursor:     %lu\r\n", _last_pushed_unix);
    Serial.printf("  success:    %lu\r\n", _push_ok_count);
    Serial.printf("  failures:   %lu\r\n", _push_fail_count);
    Serial.println("[vps] ----------------");
}
