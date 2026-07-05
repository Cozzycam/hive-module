/* VPS push — HTTPS POST colony snapshots + events to VPS. */
#include "vps_push.h"
#include "api_json.h"
#include "chores.h"
#include "coordinator.h"
#include "ota_push.h"
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

// Synchronous POST — kept for paths that must complete before a reboot.
// The periodic push cycle does NOT use it: that goes through the chores
// worker so the render loop never waits on a socket (issue #56).
[[maybe_unused]] static bool _post(const char* path, const char* body, int body_len) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);

    // Compute HMAC
    char hmac[65] = {};
    _hmac_sha256(_secret, strlen(_secret), body, body_len, hmac, sizeof(hmac));

    WiFiClient client;

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-HMAC-SHA256", hmac);
    // Until the VPS has adopted our secret, offer it for first-contact
    // enrollment. Dropped from all requests once enrolled.
    if (!_enrolled) http.addHeader("X-Enroll-Secret", _secret);
    http.setTimeout(10000);  // ms — HTTPClient sets this on the socket at connect

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

// Async POST via the chores worker: body ownership transfers to the worker,
// the HMAC + enrollment headers are computed here (main loop, ~1ms) so the
// worker never reads push state. Returns false if the worker/queue is down
// (caller treats as a failed transaction).
static uint32_t _chore_token_seq = 100;

static uint32_t _submit_post(const char* path, char* body_heap, size_t body_len) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);
    char hmac[65] = {};
    _hmac_sha256(_secret, strlen(_secret), body_heap, body_len, hmac, sizeof(hmac));
    uint32_t token = ++_chore_token_seq;
    if (!chores_submit_http(CHORE_HTTP_POST, token, url, body_heap, body_len,
                            hmac, _enrolled ? nullptr : _secret))
        return 0;
    return token;
}

static uint32_t _submit_get(const char* path) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);
    uint32_t token = ++_chore_token_seq;
    if (!chores_submit_http(CHORE_HTTP_GET, token, url, nullptr, 0,
                            nullptr, nullptr))
        return 0;
    return token;
}

// ---- HTTP GET -> JSON ----

static bool _get_json(const char* path, JsonDocument& doc) {
    char url[196];
    snprintf(url, sizeof(url), "%s%s", _endpoint, path);

    WiFiClient client;

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.begin(client, url);
    http.setTimeout(10000);  // ms — HTTPClient sets this on the socket at connect

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

static constexpr uint32_t OTA_HTTP_TIMEOUT_MS  = 10000;   // connect + per-read socket bound
static constexpr uint32_t OTA_STALL_TIMEOUT_MS = 20000;   // zero bytes for this long = dead link
static constexpr uint32_t OTA_DEADLINE_MS      = 180000;  // whole-download hard cap
static constexpr size_t   OTA_CHUNK            = 4096;
static constexpr uint8_t  OTA_MAX_ATTEMPTS     = 3;       // flash-window tries per image (md5)

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

    // 3.5 Bound the retries. Every failure past the splash below reboots
    // WITHOUT acking the ota_update command, so a deterministic failure —
    // classic case: firmware.bin redeployed without regenerating latest.json,
    // so the signed md5 no longer matches the served file — would re-trigger
    // the identical splash-download-reboot cycle on every 30s poll, forever,
    // re-applying every other command stuck in the same unacked batch each
    // time. Count flash-window attempts per image (keyed by manifest md5) in
    // NVS; once an image has burned OTA_MAX_ATTEMPTS we refuse it and return
    // false so _poll_commands finally acks the command. Fixing the manifest
    // changes the md5, which resets the count. Deliberately NOT cleared on
    // success: if the new image flunks its boot trial and rolls back, the
    // still-pending command re-flashes the same md5 — bounded by this same
    // counter instead of looping flash-rollback-flash forever.
    uint8_t tries = 0;
    {
        Preferences p;
        p.begin("ota", true);
        char tried[33] = {};
        p.getString("try_md5", tried, sizeof(tried));
        if (strncmp(tried, md5, 32) == 0) tries = p.getUChar("try_n", 0);
        p.end();
    }
    if (tries >= OTA_MAX_ATTEMPTS) {
        Serial.printf("[ota] image %.8s... already failed %u attempts — refusing until the manifest changes\r\n",
                      md5, (unsigned)tries);
        return false;
    }

    // 4. Download + flash, verifying the authenticated MD5.
    // Hand-rolled copy loop instead of Update.writeStream(): on a WiFi stall
    // writeStream retries zero-byte reads 300x against the full stream
    // timeout — up to ~100 minutes of frozen screen with no serial and no
    // watchdog (the v186->v187 wedge). Here every call is bounded (reads are
    // non-blocking, flash writes are ms) and the whole download has a hard
    // deadline, so a stall can never freeze the UI past OTA_DEADLINE_MS.
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
    http.begin(client, url);
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);  // ms — applied to the socket at connect
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

    // Record the attempt BEFORE the flash window opens: from the splash on,
    // failure exits reboot without returning, so NVS is the only memory of
    // how many times this image has already burned us.
    {
        Preferences p;
        p.begin("ota", false);
        p.putString("try_md5", md5);
        p.putUChar("try_n", (uint8_t)(tries + 1));
        p.end();
    }

    // Screen freezes for the whole download — tell the keeper it's deliberate.
    // From the splash on, every failure exit REBOOTS instead of returning:
    // the running image is untouched (a half-written inactive partition is
    // safe to abandon) and rebooting is the only way back to a sane screen.
    // Retries after such a reboot are bounded by the attempt counter above.
    char note[48];
    snprintf(note, sizeof(note), "new firmware v%lu -> v%lu",
             (unsigned long)FW_VERSION, (unsigned long)version);
    ota_splash(note);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t* buf = (uint8_t*)malloc(OTA_CHUNK);
    uint32_t start_ms = millis();
    uint32_t last_data_ms = millis();
    size_t written = 0;
    const char* fail = buf ? nullptr : "no memory for download buffer";
    while (!fail && written < (size_t)len) {
        if (millis() - start_ms > OTA_DEADLINE_MS) { fail = "deadline (3 min)"; break; }
        int avail = stream->available();
        if (avail <= 0) {
            if (!client.connected()) { fail = "connection lost"; break; }
            if (millis() - last_data_ms > OTA_STALL_TIMEOUT_MS) { fail = "stalled"; break; }
            delay(10);
            continue;
        }
        size_t want = (size_t)avail;
        if (want > OTA_CHUNK) want = OTA_CHUNK;
        if (want > (size_t)len - written) want = (size_t)len - written;
        int n = stream->read(buf, want);
        if (n < 0) { fail = "socket error"; break; }
        if (n > 0) {
            if (Update.write(buf, (size_t)n) != (size_t)n) { fail = Update.errorString(); break; }
            written += (size_t)n;
            last_data_ms = millis();
        }
    }
    free(buf);
    http.end();

    if (fail) {
        Serial.printf("[ota] download aborted (%s) at %u/%d bytes — rebooting clean\r\n",
                      fail, (unsigned)written, len);
        Update.abort();
        delay(200);
        ESP.restart();
    }
    if (!Update.end(true)) {
        Serial.printf("[ota] verify/commit failed: %s — rebooting clean\r\n",
                      Update.errorString());
        delay(200);
        ESP.restart();
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

static uint32_t _submit_commands_poll(Coordinator& coord) {
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/commands/pending",
             coord.registry.manifest().colony_id);
    return _submit_get(path);
}

// Apply the pending-commands response (worker GET result). Commands mutate
// the coordinator, so this always runs on the main loop. Returns the chore
// token of the ack POST, or 0 when there was nothing to ack.
static uint32_t _handle_commands_body(Coordinator& coord, const char* body) {
    JsonDocument doc;
    if (!body || deserializeJson(doc, body)) return 0;

    JsonArray results = doc["results"];
    if (results.isNull() || results.size() == 0) return 0;

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
            // On success this reboots into the new image; the command stays
            // pending until the post-reboot poll reads "already current" and
            // acks it then. Failures before the update splash return false
            // and ack now; failures after it reboot WITHOUT acking, so the
            // command re-triggers on the next poll — bounded by the per-image
            // attempt counter in _ota_from_vps (OTA_MAX_ATTEMPTS), after
            // which it refuses the image and the ack finally lands. Note the
            // retried batch re-applies any commands queued alongside this
            // one, so that bound also caps their re-execution.
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

    if (first) return 0;   // nothing valid to ack

    char path[96];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/commands/ack",
             coord.registry.manifest().colony_id);
    char* ack_body = (char*)malloc(acks.length() + 1);
    if (!ack_body) { _pending_convert = false; return 0; }
    memcpy(ack_body, acks.c_str(), acks.length() + 1);
    return _submit_post(path, ack_body, acks.length());
}

// The ack result closes the cycle; a pending satellite conversion fires
// only once its ack has landed (same ordering as the old synchronous path).
static void _handle_ack_result(Coordinator& coord, bool acked) {
    if (_pending_convert && acked) {
        Serial.printf("[vps] converting to %s — wiping colony, rebooting\r\n",
                      module_role_str(_pending_convert_role));
        chores_drain();   // queued record writes must land before the wipe
        colony_reset_wipe();
        coord.set_role_nvs((ModuleRole)_pending_convert_role);
        delay(200);
        ESP.restart();
    }
    _pending_convert = false;
}

// ---- Public API ----

void vps_push_init() {
    _load_config();
    _last_push_ms = millis();
}

// The push cycle: payloads are built on the main loop (fast, pure CPU) and
// the HTTP round trips run on the chores worker (core 0). Each WAIT phase
// holds until the worker's result comes back through
// vps_push_handle_result — the loop never blocks on a socket. (History:
// v198 split the three transactions across loop passes to stop satellite
// heartbeat flaps; each pass still froze the glass 150-460ms — the
// remaining half of issue #56.)
enum PushPhase : uint8_t {
    PUSH_IDLE = 0, PUSH_SNAP_WAIT, PUSH_EV_WAIT, PUSH_CMD_WAIT, PUSH_ACK_WAIT,
};
static uint8_t  _push_phase = PUSH_IDLE;
static uint32_t _phase_token = 0;        // chore token the phase waits on
static uint32_t _phase_started_ms = 0;
static constexpr uint32_t PHASE_DEADLINE_MS = 45000;  // worker timeouts are 10s;
                                                      // this is a lost-result backstop

// Event-cursor candidates — committed only when the worker reports the POST
// landed (same semantics as the old synchronous path).
static uint32_t _cand_appended = 0;
static bool     _cand_capped = false;
static uint32_t _cand_today = 0;

static uint32_t _push_snapshot(Coordinator& coord) {
    // Buffer sized for roster: ~100 bytes per conker
    size_t buf_size = 4096 + coord.registry.living_count() * 512;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return 0;

    size_t len = api_colony_json(coord, buf, buf_size);
    if (len == 0) { free(buf); return 0; }
    char path[80];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/snapshot",
             coord.registry.manifest().colony_id);
    return _submit_post(path, buf, len);   // buf ownership -> worker
}

// Build the events batch and submit it. Returns the chore token to wait on,
// or 0 when there's nothing in flight (no new lines / submit failed) — the
// no-lines case still advances the catch-up day inline.
static uint32_t _push_events(Coordinator& coord) {
    // Push events since the line cursor (16KB of new lines per cycle; a
    // backlog drains across cycles instead of silently truncating). The
    // cursor day only rolls forward once its file is FULLY drained, so a
    // day spent offline catches up from the 32GB SD archive when WiFi
    // returns — history is never dropped at midnight.
    if (g_tod.unix_time == 0) return 0;

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
        return 0;
    }

    if (!ctx.has) {
        // Nothing new. A drained PAST day still advances toward today.
        if (!ctx.capped && _pushed_day < today) {
            _pushed_day++;
            _pushed_lines = 0;
            _save_cursor();
            Serial.printf("[vps] journal catch-up: advancing to day %lu\r\n",
                          (unsigned long)_pushed_day);
        }
        return 0;
    }

    char path[80];
    snprintf(path, sizeof(path), "/api/v1/colonies/%s/events",
             coord.registry.manifest().colony_id);
    char* body = (char*)malloc(events.length() + 1);
    if (!body) return 0;
    memcpy(body, events.c_str(), events.length() + 1);

    uint32_t token = _submit_post(path, body, events.length());
    if (token) {
        // Stash the cursor candidate; committed in the result handler
        _cand_appended = ctx.appended;
        _cand_capped = ctx.capped;
        _cand_today = today;
    }
    return token;
}

// Commit the events cursor once the worker confirms the POST landed.
static void _commit_events_cursor(bool sent_ok) {
    if (sent_ok) {
        _last_pushed_unix = g_tod.unix_time;
        _pushed_lines += _cand_appended;
        bool advanced = false;
        if (!_cand_capped && _pushed_day < _cand_today) {
            _pushed_day++;
            _pushed_lines = 0;
            advanced = true;
            Serial.printf("[vps] journal catch-up: advancing to day %lu\r\n",
                          (unsigned long)_pushed_day);
        }
        (void)advanced;
        _save_cursor();
    }
    _cand_appended = 0;
    _cand_capped = false;
}

// Advance to the next phase, submitting its work. Phases that have nothing
// to send fall straight through to the next (so an empty events batch
// doesn't stall the cycle a full result round-trip).
static void _enter_phase(Coordinator& coord, uint8_t phase) {
    _push_phase = phase;
    _phase_started_ms = millis();

    switch (phase) {
    case PUSH_SNAP_WAIT:
        _phase_token = _push_snapshot(coord);
        if (!_phase_token) _enter_phase(coord, PUSH_EV_WAIT);
        return;
    case PUSH_EV_WAIT:
        _phase_token = _push_events(coord);
        if (!_phase_token) _enter_phase(coord, PUSH_CMD_WAIT);
        return;
    case PUSH_CMD_WAIT:
        _phase_token = _submit_commands_poll(coord);
        if (!_phase_token) _push_phase = PUSH_IDLE;
        return;
    case PUSH_ACK_WAIT:
        // token set by the caller (ack submit)
        return;
    default:
        _push_phase = PUSH_IDLE;
        return;
    }
}

void vps_push_tick(Coordinator& coord) {
    if (!_configured || !WiFi.isConnected()) return;

    if (_push_phase == PUSH_IDLE) {
        if (millis() - _last_push_ms < PUSH_INTERVAL_MS) return;
        _last_push_ms = millis();
        _enter_phase(coord, PUSH_SNAP_WAIT);
        return;
    }

    // Lost-result backstop: results normally always arrive (the worker's
    // HTTP timeouts are 10s), but never wedge the cycle on a missing one.
    if (millis() - _phase_started_ms > PHASE_DEADLINE_MS) {
        Serial.printf("[vps] phase %u timed out waiting for worker — resetting\r\n",
                      _push_phase);
        if (_push_phase == PUSH_EV_WAIT) _commit_events_cursor(false);
        _push_phase = PUSH_IDLE;
        _phase_token = 0;
    }
}

// Worker results, routed here from the main loop's chores pump. Drives the
// phase machine that vps_push_tick started.
void vps_push_handle_result(Coordinator& coord, uint32_t token, int status,
                            const char* body) {
    bool ok = status >= 200 && status < 300;

    // Shared POST bookkeeping (counters + first-contact enrollment), same
    // semantics as the synchronous _post
    if (ok) {
        _push_ok_count++;
        if (!_enrolled) {
            _enrolled = true;
            Preferences prefs;
            prefs.begin("vps", false);
            prefs.putBool("enrolled", true);
            prefs.end();
            Serial.println("[vps] enrolled with VPS");
        }
    } else {
        _push_fail_count++;
        Serial.printf("[vps] async HTTP failed (status %d, phase %u)\r\n",
                      status, _push_phase);
    }

    if (token != _phase_token) return;   // stale result from an abandoned phase
    _phase_token = 0;

    switch (_push_phase) {
    case PUSH_SNAP_WAIT:
        _enter_phase(coord, PUSH_EV_WAIT);
        return;
    case PUSH_EV_WAIT:
        _commit_events_cursor(ok);
        _enter_phase(coord, PUSH_CMD_WAIT);
        return;
    case PUSH_CMD_WAIT: {
        uint32_t ack_token = ok ? _handle_commands_body(coord, body) : 0;
        if (ack_token) {
            _enter_phase(coord, PUSH_ACK_WAIT);
            _phase_token = ack_token;
        } else {
            _pending_convert = false;   // no ack in flight — never convert unacked
            _push_phase = PUSH_IDLE;
        }
        return;
    }
    case PUSH_ACK_WAIT:
        _handle_ack_result(coord, ok);
        _push_phase = PUSH_IDLE;
        return;
    default:
        return;
    }
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
