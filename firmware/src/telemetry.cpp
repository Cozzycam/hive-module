/* TelemetryLog — see telemetry.h. */
#include "telemetry.h"
#include "chamber.h"
#include "conker.h"
#include "config.h"
#include "time_of_day.h"
#include "sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <time.h>

TelemetryLog g_telemetry;

// mood index -> ConkerMood order; need index -> NeedDim order; phase -> DayPhase.
static const char* TM_MOOD[]  = { "content","restless","bored","playing","sleepy","happy","lonely" };
static const char* TM_NEED[]  = { "boredom","social","rest" };   // NeedDim: BOREDOM,SOCIAL,REST
static const char* TM_PHASE[] = { "night","dawn","day","dusk" };

static const char* TELEM_DIR = "/colony/telemetry";

static const char* CSV_HEADER =
    "unix,uptime_ms,module,id,name,role,state,mood,boredom,tiredness,loneliness,"
    "hunger,intent,sleeping,phase,night_factor,x,y\n";

void TelemetryLog::init() {
    { Preferences p; p.begin("telemetry", true);
      _enabled = p.getBool("on", true); p.end(); }

    if (sd_card_state() != SD_OK) { _ready = false; return; }
    if (!SD_MMC.exists(TELEM_DIR)) SD_MMC.mkdir(TELEM_DIR);
    _ready   = true;
    _last_ms = millis();
    Serial.printf("[telemetry] %s (10s sampling)\r\n", _enabled ? "ON" : "OFF");
}

void TelemetryLog::set_enabled(bool on) {
    _enabled = on;
    Preferences p; p.begin("telemetry", false);
    p.putBool("on", on); p.end();
    Serial.printf("[telemetry] %s\r\n", on ? "ON" : "OFF");
}

void TelemetryLog::_today_path(char* buf, size_t buflen) {
    // No clock yet (pre-NTP / pre-state-sync): park rows in a dateless file —
    // they still carry uptime_ms so they're not useless.
    if (g_tod.unix_time == 0) {
        snprintf(buf, buflen, "%s/nodate.csv", TELEM_DIR);
        return;
    }
    time_t tt = (time_t)g_tod.unix_time;
    struct tm t; gmtime_r(&tt, &t);
    snprintf(buf, buflen, "%s/%04d-%02d-%02d.csv",
             TELEM_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

void TelemetryLog::tick(const Chamber& ch, uint16_t module_id) {
    if (!_enabled || !_ready) return;
    if (sd_card_state() != SD_OK) return;
    uint32_t now = millis();
    if (now - _last_ms < TELEMETRY_INTERVAL_MS) return;
    _last_ms = now;
    _sample(ch, module_id);
}

void TelemetryLog::_sample(const Chamber& ch, uint16_t module_id) {
    char path[48];
    _today_path(path, sizeof(path));

    bool fresh = !SD_MMC.exists(path);   // write header on a brand-new day file
    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) { sd_card_write_failed(); return; }
    if (fresh) f.print(CSV_HEADER);

    uint32_t unix     = g_tod.unix_time;
    uint32_t up       = millis();
    const char* phase = TM_PHASE[g_tod.phase & 3];

    char row[192];
    for (int i = 0; i < ch.conker_count; i++) {
        const Conker& c = ch.conkers[i];
        if (!c.alive) continue;
        const char* mood   = TM_MOOD[c.mood < MOOD_COUNT ? c.mood : 0];
        const char* intent = c.intent_need < NEED_COUNT ? TM_NEED[c.intent_need] : "none";
        snprintf(row, sizeof(row),
            "%lu,%lu,%04X,%lu,%s,%u,%u,%s,%.3f,%.3f,%.3f,%.2f,%s,%u,%s,%.2f,%.2f,%.2f\n",
            (unsigned long)unix, (unsigned long)up, module_id,
            (unsigned long)c.id, c.name[0] ? c.name : "?",
            (unsigned)c.role, (unsigned)c.state, mood,
            c.needs[NEED_BOREDOM], c.needs[NEED_REST], c.needs[NEED_SOCIAL],
            c.hunger, intent, (unsigned)(c.sleeping ? 1 : 0),
            phase, g_tod.night_factor, c.x, c.y);
        f.print(row);
    }
    f.flush();
    f.close();
    sd_card_write_ok();
}

size_t TelemetryLog::dump_today() {
    char path[48];
    _today_path(path, sizeof(path));
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { Serial.printf("[telemetry] no file: %s\r\n", path); return 0; }
    size_t total = f.size();
    Serial.printf("=== TELEMETRY %s %u bytes ===\r\n", path, (unsigned)total);
    uint8_t buf[256];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n > 0) Serial.write(buf, n);
    }
    f.close();
    Serial.println("\r\n=== END ===");
    return total;
}

void TelemetryLog::print_status() {
    char path[48];
    _today_path(path, sizeof(path));
    Serial.printf("[telemetry] %s, interval 10s\r\n", _enabled ? "ON" : "OFF");
    Serial.printf("[telemetry] file: %s\r\n", path);
    if (sd_card_state() == SD_OK) {
        File f = SD_MMC.open(path, FILE_READ);
        if (f) { Serial.printf("[telemetry] size: %u bytes\r\n", (unsigned)f.size()); f.close(); }
        else Serial.println("[telemetry] (no file yet)");
    }
}
