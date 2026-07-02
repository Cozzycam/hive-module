/*
 * Hive Module — Live colony simulation on ESP32-S3.
 *
 * Serial commands (via device monitor):
 *   +/f  -- faster sim speed       -/s  -- slower sim speed
 *   1-7  -- set speed directly     (2,5,8,15,30,60,150 tps)
 *   z    -- zoom in                x    -- zoom out
 *   ?    -- print current status
 *   R    -- reboot ESP32
 *   role queen     -- set module role to queen (NVS) and reboot
 *   role satellite -- set module role to satellite (NVS) and reboot
 *   d              -- toggle topology debug overlay
 *   ota            -- enter OTA mode (queen only, 2 min window, reboots on exit)
 *   push           -- push firmware to satellites over WiFi (queen only, reboots)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <esp_random.h>

#include "pin_config.h"
#include "config.h"
#include "rng.h"
#include "sim.h"
#include "renderer.h"
#include "touch.h"
#include "events.h"
#include "time_of_day.h"
#include "weather.h"
#include "hud.h"
#include "names.h"
#include "topology.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "ota_push.h"
#include "sd_card.h"
#include "world_condition.h"
#include "api_json.h"
#include "telemetry.h"
#include "http_server.h"
#include "vps_push.h"
#include "setup_wizard.h"
#include <SD_MMC.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// Event type names for serial logging
static const char* EVT_NAMES[] = {
    "interaction_started", "interaction_ended",
    "food_delivered", "food_tapped", "pile_discovered",
    "queen_laid_egg", "young_hatched", "young_died",
    "conker_died", "handoff_incoming", "handoff_outgoing",
    "parade_started",
};

// -- TCA9554 I/O expander --------------------------------------------

static void tca9554_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}
static uint8_t tca9554_read_reg(uint8_t reg) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    return Wire.read();
}
static void lcd_reset() {
    // Set P1 as output
    uint8_t cfg = tca9554_read_reg(0x03);
    tca9554_write_reg(0x03, cfg & ~(1 << TCA9554_LCD_RST));
    // Toggle reset
    uint8_t out = tca9554_read_reg(0x01);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out & ~(1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(200);
}

// -- Display ---------------------------------------------------------

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK,
    LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_TFT *panel = new Arduino_AXS15231B(
    bus, -1, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// -- Sim + Renderer --------------------------------------------------

static Sim sim;
static Renderer renderer;
static bool topo_overlay = false;  // debug overlay toggle
static bool force_daytime = false; // debug: lock to midday

// Topology callback trampoline (sim is file-static, safe to reference)
static void topo_callback(Face face, bool connected, uint16_t module_id) {
    sim.coordinator.on_topology_change(face, connected, module_id);
}

// -- Speed control ---------------------------------------------------

static const int SPEED_LEVELS[] = {2, 5, 8, 15, 30, 60, 150, 300, 600, 1000};
static const int NUM_SPEEDS = sizeof(SPEED_LEVELS) / sizeof(SPEED_LEVELS[0]);
static int speed_index = 2;  // default = 8 tps

static unsigned long tick_interval_ms() {
    return 1000 / SPEED_LEVELS[speed_index];
}

// -- Time-warp tuning mode (see config.h; `warp` serial cmd) ----------
bool g_warp_active = false;             // declared extern in time_of_day.h
static uint32_t g_warp_stop_unix = 0;   // sim-time at which warp auto-stops

// -- Timing ----------------------------------------------------------

static unsigned long last_tick_ms = 0;
static unsigned long last_frame_ms = 0;

// -- Death tracking (queryable via serial 'deaths') ----------------------
uint16_t g_deaths_starved = 0;
uint16_t g_deaths_old_age = 0;

// -- Handoff tracking (queryable via serial 'deaths') --------------------
uint32_t g_handoffs_out = 0;
uint32_t g_handoffs_in = 0;
uint32_t g_handoffs_dropped = 0;

// -- OTA mode (queen only) -------------------------------------------

static void enter_ota_mode() {
    if (!sim.coordinator.is_queen()) {
        Serial.println("[ota] Only available on queen");
        return;
    }

    Serial.println("[ota] Entering OTA mode (topology will disconnect)...");

    if (!tod_wifi_connect(15000)) {
        Serial.println("[ota] WiFi failed — aborting");
        return;
    }

    ArduinoOTA.setHostname("hive-queen");
    ArduinoOTA.onStart([]() {
        Serial.println("[ota] Upload starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[ota] Upload complete — rebooting");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[ota] %u%%\r", progress / (total / 100));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[ota] Error[%u] — rebooting to restore topology\r\n", error);
        delay(100);
        ESP.restart();
    });
    ArduinoOTA.begin();

    Serial.printf("[ota] Ready. Upload with:\r\n");
    Serial.printf("  pio run -t upload --upload-port %s\r\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[ota] Waiting 2 min... press 'q' to cancel (will reboot)\r\n");
    Serial.printf("[ota] FW_VERSION=%lu\r\n", (unsigned long)FW_VERSION);

    uint32_t start = millis();
    while (millis() - start < 120000) {
        ArduinoOTA.handle();
        if (Serial.available() && Serial.read() == 'q') {
            Serial.println("[ota] Cancelled — rebooting to restore topology");
            delay(100);
            ESP.restart();
        }
        delay(10);
    }

    // Timeout — reboot to cleanly restore ESP-NOW + topology
    Serial.println("[ota] Timeout — rebooting to restore topology");
    delay(100);
    ESP.restart();
}

// -- Serial command handling -----------------------------------------

static void print_status() {
    Serial.printf(
        "Pop %d | Food %.0f | E%d L%d P%d | "
        "Pressure %.2f | Speed %d tps\r\n",
        sim.coordinator.colony.population, sim.coordinator.colony.food_total,
        sim.coordinator.colony.brood_egg, sim.coordinator.colony.brood_larva, sim.coordinator.colony.brood_pupa,
        sim.coordinator.colony.food_pressure(),
        SPEED_LEVELS[speed_index]);
}

// Line buffer for multi-word serial commands (e.g. "role queen")
static char serial_buf[64];
static int  serial_len = 0;

// Debug fast-forward: while nonzero, founder brood timers are clamped so
// eggs hatch immediately, until the local chamber reaches this many workers.
// Uses the natural lay→hatch path (identity, names, journal all real).
static uint32_t g_ff_target_workers = 0;

static void process_serial_line(const char* line) {
    // Multi-word commands
    if (strncmp(line, "role ", 5) == 0) {
        int r = module_role_from_str(line + 5);
        if (r >= MODULE_QUEEN) {
            Coordinator::set_role_nvs(static_cast<ModuleRole>(r));
            Serial.printf("Rebooting as %s...\r\n", line + 5);
            delay(100);
            ESP.restart();
        } else {
            Serial.println("Unknown role (queen|satellite|garden|food_store|heart_tree)");
        }
    } else if (strncmp(line, "tint ", 5) == 0) {
        // tint <r> <g> <b>  |  tint off
        if (strcmp(line + 5, "off") == 0) {
            renderer_set_floor_tint(0, 0, 0, true);
        } else {
            int r = 0, g = 0, b = 0;
            if (sscanf(line + 5, "%d %d %d", &r, &g, &b) == 3)
                renderer_set_floor_tint((uint8_t)r, (uint8_t)g, (uint8_t)b, true);
            else
                Serial.println("Usage: tint <r> <g> <b> | tint off");
        }
    } else if (strcmp(line, "factory") == 0) {
        // Factory reset: clear role + WiFi creds (keeps VPS provisioning),
        // wipe colony data on SD, reboot into the setup wizard.
        Serial.println("Factory reset: clearing role, WiFi, colony data...");
        {
            Preferences p;
            p.begin("hive", false); p.clear(); p.end();
            p.begin("wifi", false); p.clear(); p.end();
        }
        if (sd_card_state() == SD_OK) {
            sd_remove_recursive("/colony");
        }
        Serial.println("Rebooting into setup wizard...");
        delay(200);
        ESP.restart();
    } else if (strcmp(line, "ota") == 0) {
        enter_ota_mode();
    } else if (strcmp(line, "clearbonds") == 0) {
        // Wipe all friendships (RAM + SD) so they reform cleanly under the
        // current bonding rules. Lineage bonds re-set on the next hatch.
        sim.coordinator.bonds.init();
        if (sd_card_state() == SD_OK) {
            SD_MMC.remove("/colony/bonds.json");
            SD_MMC.remove("/colony/bonds.json.tmp");
        }
        Serial.println("[bonds] cleared (RAM + SD) — friendships will reform");
    } else if (strcmp(line, "blankslate") == 0) {
        // Tuning baseline: zero every conker's needs/mood/sleep, wipe bonds
        // (RAM + SD), and clear the telemetry CSVs — a clean slate for a fresh
        // observation run. Run on BOTH modules. Hunger is zeroed too (well-fed
        // start); lineage bonds re-set on the next hatch.
        auto& cham = sim.coordinator.chamber;
        int nc = 0;
        for (int i = 0; i < cham.conker_count; i++) {
            Conker& c = cham.conkers[i];
            for (int k = 0; k < NEED_COUNT; k++) c.needs[k] = 0.0f;
            c.mood            = MOOD_CONTENT;
            c.intent_need     = NEED_COUNT;
            c.afterglow_ticks = 0;
            c.hunger          = 0.0f;
            c.sleeping        = false;
            c.sleep_until_ms  = 0;
            c.daytime_nap     = false;
            c.seeking_company = false;
            c.seek_ticks      = 0;
            nc++;
        }
        sim.coordinator.bonds.init();
        if (sd_card_state() == SD_OK) {
            SD_MMC.remove("/colony/bonds.json");
            SD_MMC.remove("/colony/bonds.json.tmp");
        }
        int tf = g_telemetry.reset_files();
        Serial.printf("[blankslate] %d conkers reset, bonds cleared, %d telemetry file(s) removed\r\n",
                      nc, tf);
    } else if (strcmp(line, "push") == 0) {
        ota_push();
    } else if (strcmp(line, "deaths") == 0) {
        Serial.printf("Deaths: %d starved, %d old age | Handoffs: %lu out, %lu in, %lu dropped\r\n",
            g_deaths_starved, g_deaths_old_age,
            (unsigned long)g_handoffs_out, (unsigned long)g_handoffs_in,
            (unsigned long)g_handoffs_dropped);
    } else if (strcmp(line, "weather") == 0) {
        if (g_weather.valid) {
            static const char* wx[] = {"clear","partly cloudy","overcast","fog","drizzle","rain","heavy rain","snow","thunderstorm"};
            static const char* tp[] = {"freezing","cold","mild","warm","hot","extreme heat"};
            static const char* wn[] = {"calm","breezy","windy","high wind","storm"};
            Serial.printf("[weather] %s, %.1fC (%s), wind %.0f km/h (%s)\r\n",
                wx[g_weather.condition], g_weather.temperature_c, tp[g_weather.temp],
                g_weather.wind_speed_kmh, wn[g_weather.wind]);
        } else {
            Serial.println("[weather] no data yet");
        }
    } else if (strncmp(line, "wx ", 3) == 0) {
        const char* arg = line + 3;
        struct { const char* name; WeatherCondition c; } wxmap[] = {
            {"clear", WX_CLEAR}, {"cloudy", WX_PARTLY_CLOUDY},
            {"overcast", WX_OVERCAST}, {"fog", WX_FOG},
            {"drizzle", WX_DRIZZLE}, {"rain", WX_RAIN},
            {"heavy", WX_HEAVY_RAIN}, {"snow", WX_SNOW},
            {"storm", WX_THUNDERSTORM},
        };
        for (auto& w : wxmap) {
            if (strcmp(arg, w.name) == 0) {
                g_weather.condition = w.c;
                g_weather.valid = true;
                Serial.printf("[weather] forced: %s\r\n", w.name);
                break;
            }
        }
    } else if (strcmp(line, "kill-radio") == 0) {
        Serial.println("[test] Killing WiFi radio to simulate ESP-NOW death...");
        WiFi.mode(WIFI_OFF);
        Serial.println("[test] WiFi.mode(WIFI_OFF) — ESP-NOW is now dead. Watch for reinit.");
    } else if (strcmp(line, "dump colony") == 0) {
        char* buf = (char*)malloc(4096);
        if (buf) {
            size_t len = api_colony_json(sim.coordinator, buf, 4096);
            Serial.println(buf);
            free(buf);
        }
    } else if (strcmp(line, "dump lilguys") == 0) {
        char* buf = (char*)malloc(8192);
        if (buf) {
            size_t len = api_lilguys_json(sim.coordinator, buf, 8192, 100, 0);
            Serial.println(buf);
            free(buf);
        }
    } else if (strncmp(line, "dump lilguy ", 12) == 0) {
        uint32_t id = atoi(line + 12);
        char* buf = (char*)malloc(2048);
        if (buf) {
            size_t len = api_lilguy_detail_json(sim.coordinator, id, buf, 2048);
            if (len > 0) Serial.println(buf);
            else Serial.printf("[api] lilguy %lu not found\r\n", (unsigned long)id);
            free(buf);
        }
    } else if (strcmp(line, "dump bonds") == 0) {
        // Read-only diagnostic: every bond in the pool, INCLUDING the
        // sub-threshold (unformed) ones the snapshot/API hide. Lets us see who's
        // accruing affinity but hasn't crossed FORM_THRESHOLD (0.1) yet vs. who
        // never started at all. Columns: owner -> target  strength  F(ormed) R(ecip).
        auto& bs = sim.coordinator.bonds;
        auto& reg = sim.coordinator.registry;
        const auto* pool = bs.pool();
        int n = bs.count();
        Serial.printf("[bonds] %d entries  (owner -> target  strength  F R)\r\n", n);
        for (int i = 0; i < n; i++) {
            const auto& e = pool[i];
            auto* ro = reg.get(e.owner);
            auto* rt = reg.get(e.target);
            bool recip = bs.is_formed(e.target, e.owner);
            Serial.printf("  %-12s(%lu) -> %-12s(%lu)  %.3f  %c %c\r\n",
                ro ? ro->name : "?", (unsigned long)e.owner,
                rt ? rt->name : "?", (unsigned long)e.target,
                e.strength, e.formed ? 'F' : '.', recip ? 'R' : '.');
        }
    } else if (strcmp(line, "brood") == 0) {
        auto& cham = sim.coordinator.chamber;
        Serial.printf("[brood] %d in nest\r\n", cham.brood_count);
        for (int i = 0; i < cham.brood_count; i++) {
            Brood& b = cham.brood[i];
            if (!b.alive()) continue;
            uint32_t egg_dur, seed_dur;
            if (b.total_duration_ms > 0) {
                egg_dur  = b.total_duration_ms / 3;
                seed_dur = b.total_duration_ms - egg_dur;
            } else {
                egg_dur  = (uint32_t)(Cfg::EGG_DURATION_DAYS  * Cfg::SECS_PER_DAY * 1000.0f);
                seed_dur = (uint32_t)(Cfg::SEED_DURATION_DAYS * Cfg::SECS_PER_DAY * 1000.0f);
            }
            uint32_t elapsed = millis() - b.stage_start_ms;
            uint32_t dur = (b.stage == STAGE_EGG) ? egg_dur : seed_dur;
            float pct = dur ? (100.0f * elapsed / dur) : 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            float remain_h = (dur > elapsed) ? (dur - elapsed) / 3600000.0f : 0.0f;
            const char* st = (b.stage == STAGE_EGG) ? "EGG" : "SEED/larva";
            Serial.printf("  id=%-3lu %-10s %.1f%% through stage (%.1fh left)  food=%.1f  hunger=%.2f  @(%d,%d)\r\n",
                (unsigned long)b.id, st, pct, remain_h,
                b.food_invested, b.hunger, b.x, b.y);
        }
    } else if (strcmp(line, "hatchall") == 0) {
        // Admin: insta-hatch every live brood (rides the normal hatch pipeline
        // on the next tick) and lay one fresh egg beside the queen.
        auto& cham = sim.coordinator.chamber;
        int forced = 0;
        for (int i = 0; i < cham.brood_count; i++) {
            Brood& b = cham.brood[i];
            if (!b.alive()) continue;
            uint32_t seed_dur = (b.total_duration_ms > 0)
                ? (b.total_duration_ms - b.total_duration_ms / 3)
                : (uint32_t)(Cfg::SEED_DURATION_DAYS * Cfg::SECS_PER_DAY * 1000.0f);
            b.stage          = STAGE_SEED;
            b.food_invested  = Cfg::SEED_TOTAL_FOOD;   // satisfy the food gate
            b.hunger         = 0.0f;
            b.stage_start_ms = millis() - seed_dur - 1000;  // elapsed just past full term
            forced++;
        }
        bool laid = false;
        if (cham.has_queen && cham.queen_obj.alive && cham.brood_count < Cfg::MAX_BROOD) {
            int ex = cham.queen_obj.x + 3, ey = cham.queen_obj.y + 2;
            if (ex > Cfg::GRID_WIDTH  - 3) ex = Cfg::GRID_WIDTH  - 3;
            if (ey > Cfg::GRID_HEIGHT - 3) ey = Cfg::GRID_HEIGHT - 3;
            if (cham.in_bounds(ex, ey)) { cham.add_brood(ex, ey); laid = true; }
        }
        Serial.printf("[hatchall] %d brood set to hatch next tick, fresh egg laid: %s\r\n",
                      forced, laid ? "yes" : "no");
    } else if (strcmp(line, "dump health") == 0) {
        char buf[256];
        api_health_json(sim.coordinator, buf, sizeof(buf));
        Serial.println(buf);
    } else if (strcmp(line, "names") == 0) {
        auto& reg = sim.coordinator.registry;
        IdentityRecord* recs = reg.living_records();
        Serial.println("[names] living workers:");
        for (int i = 0; i < reg.living_count(); i++) {
            Serial.printf("  id=%lu %s (role=%d pioneer=%d)\r\n",
                (unsigned long)recs[i].id, recs[i].name,
                recs[i].role, recs[i].is_pioneer);
        }
    } else if (strncmp(line, "warp", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        const char* arg = line + 4;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "off") == 0) {
            g_warp_active = false;
            last_tick_ms = millis();   // resync the wall-clock scheduler
            last_frame_ms = millis();
            Serial.printf("[warp] OFF — sim clock at local %02d:%02d (phase=%d)\r\n",
                          g_tod.local_hour, g_tod.local_minute, g_tod.phase);
        } else if (*arg == '\0' || strcmp(arg, "status") == 0) {
            Serial.printf("[warp] %s | sim local %02d:%02d phase=%d nf=%.2f | dt=%.3fs %d ticks/loop\r\n",
                g_warp_active ? "ON" : "off", g_tod.local_hour, g_tod.local_minute,
                g_tod.phase, g_tod.night_factor, Cfg::WARP_DT, Cfg::WARP_TICKS_PER_LOOP);
            if (g_warp_active)
                Serial.printf("[warp] stops at sim-unix %lu (now %lu, ~%lu sim-min left)\r\n",
                    (unsigned long)g_warp_stop_unix, (unsigned long)g_tod.unix_time,
                    (unsigned long)((g_warp_stop_unix - g_tod.unix_time) / 60));
        } else {
            float hours = (strcmp(arg, "on") == 0) ? Cfg::WARP_DEFAULT_HOURS : (float)atof(arg);
            if (hours <= 0.0f) hours = Cfg::WARP_DEFAULT_HOURS;
            g_warp_stop_unix = g_tod.unix_time + (uint32_t)(hours * 3600.0f);
            g_warp_active = true;
            Serial.printf("[warp] ON — warping %.1f sim-hours from local %02d:%02d. "
                          "Telemetry sim-gated; brood/WiFi-push paused; NTP suspended. "
                          "`warp off` to stop early.\r\n",
                          hours, g_tod.local_hour, g_tod.local_minute);
        }
    } else if (strcmp(line, "dump telemetry bonds") == 0) {
        g_telemetry.dump_bonds_today();
    } else if (strcmp(line, "dump telemetry") == 0) {
        g_telemetry.dump_today();
    } else if (strcmp(line, "telemetry on") == 0) {
        g_telemetry.set_enabled(true);
    } else if (strcmp(line, "telemetry off") == 0) {
        g_telemetry.set_enabled(false);
    } else if (strcmp(line, "telemetry reset") == 0) {
        g_telemetry.reset_files();
    } else if (strcmp(line, "telemetry") == 0) {
        g_telemetry.print_status();
    } else if (strcmp(line, "journal") == 0) {
        auto& j = sim.coordinator.journal;
        Serial.printf("[journal] pending: %d, total flushed: %d\r\n",
            j.pending_count(), j.total_flushed());
    } else if (strcmp(line, "journal dump") == 0) {
        // Flush pending events first, then dump today's file
        sim.coordinator.journal.flush();
        Serial.println("[journal] --- today's events ---");
        sim.coordinator.journal.read_day(g_tod.unix_time,
            [](const char* line, void*) -> bool {
                Serial.println(line);
                return true;
            }, nullptr);
        Serial.println("[journal] --- end ---");
    } else if (strncmp(line, "challenge start ", 16) == 0) {
        const char* arg = line + 16;
        struct { const char* name; uint8_t type; } types[] = {
            {"heatwave", CHALLENGE_HEATWAVE}, {"cold", CHALLENGE_COLD_SNAP},
            {"drought", CHALLENGE_DROUGHT}, {"storm", CHALLENGE_STORM},
        };
        uint8_t ctype = CHALLENGE_HEATWAVE;
        float severity = 0.5f;
        for (auto& t : types) {
            if (strncmp(arg, t.name, strlen(t.name)) == 0) {
                ctype = t.type;
                const char* sev = strchr(arg, ' ');
                if (sev) severity = atof(sev + 1);
                break;
            }
        }
        sim.coordinator.challenge_start(ctype, severity, sim.tick_count);
    } else if (strcmp(line, "challenge end") == 0) {
        sim.coordinator.challenge_end(sim.tick_count);
    } else if (strncmp(line, "ff ", 3) == 0) {
        g_ff_target_workers = atoi(line + 3);
        Serial.printf("[ff] fast-forwarding founder hatches until %lu workers\r\n",
                      (unsigned long)g_ff_target_workers);
    } else if (strcmp(line, "wifi status") == 0) {
        tod_wifi_status();
    } else if (strcmp(line, "wifi reconnect") == 0) {
        tod_wifi_reconnect();
    } else if (strncmp(line, "wifi ssid ", 10) == 0) {
        tod_wifi_set_ssid(line + 10);
    } else if (strncmp(line, "wifi pass ", 10) == 0) {
        tod_wifi_set_pass(line + 10);
    } else if (strcmp(line, "wifi forget") == 0) {
        tod_wifi_forget();
    } else if (strcmp(line, "wifi scan") == 0) {
        Serial.println("[wifi] Scanning...");
        topology_set_wifi_active(true);
        int n = WiFi.scanNetworks(false, false, false, 300);
        topology_set_wifi_active(false);
        if (n <= 0) {
            Serial.println("[wifi] No networks found");
        } else {
            for (int i = 0; i < n; i++) {
                Serial.printf("  %2d: ch%-2d %ddBm %s\r\n",
                    i+1, WiFi.channel(i), WiFi.RSSI(i), WiFi.SSID(i).c_str());
            }
        }
        WiFi.scanDelete();
    } else if (strncmp(line, "who ", 4) == 0) {
        // Raw record JSON straight off the SD — works for dead conkers too
        uint32_t id = strtoul(line + 4, nullptr, 10);
        static char dump[600];
        if (sim.coordinator.registry.dump_record(id, dump, sizeof(dump)))
            Serial.printf("[who] raw record %lu:\r\n%s\r\n", (unsigned long)id, dump);
        else
            Serial.printf("[who] no record file for id %lu\r\n", (unsigned long)id);
    } else if (strcmp(line, "who") == 0) {
        auto& reg = sim.coordinator.registry;
        IdentityRecord* recs = reg.living_records();
        Serial.printf("[who] %d living records, %d in chamber\r\n",
                      reg.living_count(), sim.coordinator.chamber.conker_count);
        for (int i = 0; i < reg.living_count(); i++) {
            IdentityRecord& r = recs[i];
            Serial.printf("  id=%-3lu %-14s born=%lu lived=%.2fd lifespan=%.2fd founder=%d\r\n",
                (unsigned long)r.id, r.name, (unsigned long)r.born_unix,
                r.lived_ms / 86400000.0f, r.lifespan_ms / 86400000.0f,
                r.is_founder ? 1 : 0);
        }
    } else if (strncmp(line, "tintseed ", 9) == 0) {
        // tintseed <id> <0-255> — force a conker's colour seed (live + persisted).
        // Handy for previewing the colour range: try a few values on one conker.
        char* p = (char*)line + 9;
        uint32_t id = strtoul(p, &p, 10);
        long seed = strtol(p, nullptr, 10);
        if (seed < 0) seed = 0;
        if (seed > 255) seed = 255;
        auto& cham = sim.coordinator.chamber;
        bool live = false;
        for (int i = 0; i < cham.conker_count; i++) {
            if (cham.conkers[i].id == id) {
                cham.conkers[i].tint_seed = (uint8_t)seed;
                live = true;
                break;
            }
        }
        IdentityRecord* rec = sim.coordinator.registry.get(id);
        if (rec) { rec->tint_seed = (uint8_t)seed; rec->dirty = true; }
        Serial.printf("[tintseed] id=%lu seed=%ld (live=%d record=%d)\r\n",
                      (unsigned long)id, seed, live, rec != nullptr);
    } else if (strncmp(line, "revive ", 7) == 0) {
        uint32_t id = strtoul(line + 7, nullptr, 10);
        if (sim.coordinator.registry.revive(id))
            Serial.printf("[revive] id=%lu lives again — reconcile will respawn it at the queen\r\n",
                          (unsigned long)id);
        else
            Serial.printf("[revive] could not revive id=%lu (already alive, or no record on SD)\r\n",
                          (unsigned long)id);
    } else if (strncmp(line, "hatch ", 6) == 0) {
        // Insta-hatch ONE brood by id (rides the normal hatch pipeline next tick)
        // and lay a single replacement egg, keeping the brood pipeline going.
        uint32_t id = strtoul(line + 6, nullptr, 10);
        auto& cham = sim.coordinator.chamber;
        bool found = false;
        for (int i = 0; i < cham.brood_count; i++) {
            Brood& b = cham.brood[i];
            if (b.id != id || !b.alive()) continue;
            uint32_t seed_dur = (b.total_duration_ms > 0)
                ? (b.total_duration_ms - b.total_duration_ms / 3)
                : (uint32_t)(Cfg::SEED_DURATION_DAYS * Cfg::SECS_PER_DAY * 1000.0f);
            b.stage          = STAGE_SEED;
            b.food_invested  = Cfg::SEED_TOTAL_FOOD;
            b.hunger         = 0.0f;
            b.stage_start_ms = millis() - seed_dur - 1000;
            found = true;
            break;
        }
        if (!found) {
            Serial.printf("[hatch] no live brood id=%lu\r\n", (unsigned long)id);
        } else {
            bool laid = false;
            if (cham.has_queen && cham.queen_obj.alive && cham.brood_count < Cfg::MAX_BROOD) {
                int ex = cham.queen_obj.x + 3, ey = cham.queen_obj.y + 2;
                if (ex > Cfg::GRID_WIDTH  - 3) ex = Cfg::GRID_WIDTH  - 3;
                if (ey > Cfg::GRID_HEIGHT - 3) ey = Cfg::GRID_HEIGHT - 3;
                if (cham.in_bounds(ex, ey)) { cham.add_brood(ex, ey); laid = true; }
            }
            Serial.printf("[hatch] id=%lu hatching next tick; replacement egg laid: %s\r\n",
                          (unsigned long)id, laid ? "yes" : "no");
        }
    } else if (strcmp(line, "vps status") == 0) {
        vps_push_status();
    } else if (strncmp(line, "vps secret ", 11) == 0) {
        vps_push_set_secret(line + 11);
    } else if (strncmp(line, "vps endpoint ", 13) == 0) {
        vps_push_set_endpoint(line + 13);
    } else if (strcmp(line, "reset colony") == 0) {
        colony_reset_wipe();
        Serial.println("[reset] rebooting...");
        delay(100);
        ESP.restart();
    } else if (strcmp(line, "render fullredraw on") == 0) {
        renderer.force_full_flush = true;
        Serial.println("[render] full redraw ON (dirty-rect disabled)");
    } else if (strcmp(line, "render fullredraw off") == 0) {
        renderer.force_full_flush = false;
        Serial.println("[render] full redraw OFF (dirty-rect enabled)");
    } else if (strcmp(line, "render fullredraw") == 0) {
        Serial.printf("[render] full redraw: %s\r\n", renderer.force_full_flush ? "ON" : "OFF");
    } else if (strcmp(line, "daytime") == 0) {
        force_daytime = !force_daytime;
        Serial.printf("[debug] force daytime: %s\r\n", force_daytime ? "ON" : "OFF");
    } else if (strcmp(line, "topology") == 0) {
        sim.coordinator.print_topology();
    } else if (strcmp(line, "topo status") == 0) {
        topology_status();
    } else if (strcmp(line, "sd") == 0) {
        Serial.printf("[sd] state: %s\r\n",
            sd_card_state() == SD_OK ? "OK" :
            sd_card_state() == SD_ERROR ? "ERROR" : "NOT_MOUNTED");
        if (sd_card_state() == SD_OK) {
            Serial.printf("[sd] %.1f MB total, %.1f MB used\r\n",
                sd_card_total_bytes() / (1024.0 * 1024.0),
                sd_card_used_bytes()  / (1024.0 * 1024.0));
        }
    } else if (strcmp(line, "persist") == 0) {
        auto& reg = sim.coordinator.registry;
        const char* ps[] = {"UNINIT","OK","DEGRADED","ERROR"};
        Serial.printf("[persist] state: %s\r\n", ps[reg.state()]);
        if (reg.state() == PERSIST_OK) {
            auto& m = reg.manifest();
            Serial.printf("[persist] colony_id: %s\r\n", m.colony_id);
            Serial.printf("[persist] next_id: %lu, last_tick: %lu\r\n",
                (unsigned long)m.next_lilguy_id, (unsigned long)m.last_tick);
            Serial.printf("[persist] alive: %d, brood: %d\r\n",
                reg.living_count(), reg.brood_count());
            Serial.printf("[persist] food: %.1f, workers_born: %d, census: %d\r\n",
                m.food_store, m.total_workers_born, m.worker_census);
        }
    } else if (strlen(line) == 1) {
        // Single-char commands
        char c = line[0];
        switch (c) {
        case '+': case 'f':
            if (speed_index < NUM_SPEEDS - 1) speed_index++;
            Serial.printf("Speed: %d tps\r\n", SPEED_LEVELS[speed_index]);
            break;
        case '-': case 's':
            if (speed_index > 0) speed_index--;
            Serial.printf("Speed: %d tps\r\n", SPEED_LEVELS[speed_index]);
            break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
        case '9':
            speed_index = c - '1';
            if (speed_index >= NUM_SPEEDS) speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\r\n", SPEED_LEVELS[speed_index]);
            break;
        case '0':
            speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\r\n", SPEED_LEVELS[speed_index]);
            break;
        case 'r':
            renderer.force_full_redraw();
            Serial.println("Full redraw");
            break;
        case 'R':
            Serial.println("Rebooting...");
            delay(100);
            ESP.restart();
            break;
        case '?':
            print_status();
            break;
        case 'd':
            topo_overlay = !topo_overlay;
            Serial.printf("Topology overlay: %s\r\n", topo_overlay ? "ON" : "OFF");
            if (!topo_overlay) renderer.force_full_redraw();
            break;
        case 'p':
            renderer.debug_phero = !renderer.debug_phero;
            Serial.printf("Pheromone overlay: %s\r\n", renderer.debug_phero ? "ON" : "OFF");
            if (!renderer.debug_phero) renderer.force_full_redraw();
            break;
        default:
            break;
        }
    }
}

static void handle_serial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serial_len > 0) {
                serial_buf[serial_len] = '\0';
                process_serial_line(serial_buf);
                serial_len = 0;
            }
        } else if (serial_len < (int)sizeof(serial_buf) - 1) {
            serial_buf[serial_len++] = c;
        }
    }
}

// -- Arduino entry points --------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.println("Hive Module -- live sim");
    Serial.println("Commands: +/- speed, 1-9/0 speed level, r redraw, R reboot, ? status");

    // OTA rollback guard: a freshly-flashed image is "on trial" until it proves
    // healthy (confirmed in loop() after a clean uptime). If it keeps rebooting
    // without confirming, revert to the previous image. Runs first so a crash-
    // looping image still gets its attempts counted before it can crash again.
    {
        Preferences p; p.begin("ota", false);
        if (p.getBool("trial", false)) {
            int tries = p.getInt("boot_try", 0) + 1;
            p.putInt("boot_try", tries);
            Serial.printf("[ota] trial boot attempt %d\r\n", tries);
            if (tries >= 3) {
                const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
                p.putBool("trial", false); p.putInt("boot_try", 0); p.end();
                if (prev) {
                    Serial.println("[ota] new image never confirmed — ROLLING BACK");
                    esp_ota_set_boot_partition(prev);
                }
                delay(100); ESP.restart();
            }
        }
        p.end();
    }

    g_rng = Rng(esp_random());

    Wire.begin(I2C_SDA, I2C_SCL);
    lcd_reset();

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    if (!gfx->begin()) {
        Serial.println("ERROR: display init failed");
        while (true) delay(1000);
    }
    gfx->setRotation(1);

    // SD card — mount before sim so persistence can load
    if (!sd_card_init()) {
        Serial.println("[setup] SD not available — running without persistence");
    }

    touch_init();

    // Factory-fresh module (no role in NVS): blocking setup wizard.
    // Found path provisions WiFi (captive portal) and writes role=queen,
    // so everything after this reads the right config.
    SetupChoice wizard_choice = SETUP_NONE;
    if (setup_wizard_required()) {
        wizard_choice = setup_wizard_run(gfx);
    }

    // Queen: keep WiFi connected for HTTP server + ESP-NOW channel coexistence
    // Must be set before time_of_day_init() which does the first NTP sync
    // Role isn't known yet (sim not init), so read it from NVS directly
    {
        Preferences _prefs;
        _prefs.begin("hive", true);
        uint8_t role = _prefs.getUChar("module_role", MODULE_UNCONFIGURED);
        _prefs.end();
        // Queen (1) or unconfigured (0) — same logic as is_queen()
        if (role != MODULE_SATELLITE) tod_set_persistent_wifi(true);
    }

    time_of_day_init();
    sim.init();
    topology_init(topo_callback);  // after WiFi/NTP, before renderer
    renderer.init(gfx, panel);
    renderer_load_floor_tint();  // user's ground colour, both roles

    hud_battery_init();  // AXP2101 probe — works on both queen and satellite

    g_telemetry.init();  // per-conker mood/needs CSV sampler (both roles, temporary)

    bool queen = sim.coordinator.is_queen();
    topology_set_accept_state_sync(!queen);  // queens own their own clock
    if (queen) {
        hud_init();
        hud_set_colony_name(sim.coordinator.registry.manifest().colony_id);
        hud_set_founded_unix(sim.coordinator.registry.manifest().founded_unix);
        if (wizard_choice == SETUP_FOUNDED) {
            setup_wizard_ceremony(gfx,
                sim.coordinator.registry.manifest().colony_id,
                sim.coordinator.registry.manifest().queen_name);
        }
        renderer.start_boot_splash();
        http_server_start(&sim.coordinator);
        vps_push_init();
    }

    last_tick_ms = millis();
    last_frame_ms = millis();

    Serial.println("Running.");
}

void loop() {
    unsigned long now = millis();
    unsigned long interval = tick_interval_ms();
    bool splashing = renderer.is_splash_active();

    static uint32_t last_sim_ms = 0;
    if (last_sim_ms == 0) last_sim_ms = millis();

    topology_poll();
    if (sim.coordinator.is_queen())
        weather_tick();

    // OTA: confirm a freshly-flashed image healthy once it's run cleanly for a
    // bit (cancels the rollback trial), then — on the queen — cascade the new
    // firmware to a present satellite via the existing :8266 push.
    static bool _ota_confirmed = false;
    if (!_ota_confirmed && now > 10000) {
        _ota_confirmed = true;
        Preferences p; p.begin("ota", false);
        if (p.getBool("trial", false)) {
            p.putBool("trial", false); p.putInt("boot_try", 0);
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[ota] new image confirmed healthy");
        }
        p.end();
    }
    static bool _ota_cascaded = false;
    if (!_ota_cascaded && now > 15000 && sim.coordinator.is_queen()) {
        Preferences p; p.begin("ota", false);
        bool cascade = p.getBool("cascade", false);
        if (cascade) {
            bool sat = false;
            for (int f = 0; f < FACE_COUNT; f++)
                if (topology_neighbour((Face)f).present) sat = true;
            p.putBool("cascade", false);
            if (sat) {
                p.end();
                _ota_cascaded = true;
                Serial.println("[ota] cascading new firmware to satellite...");
                ota_push();  // broadcasts announce + serves :8266 (blocks, reboots)
            } else {
                p.end();
                _ota_cascaded = true;
            }
        } else { p.end(); _ota_cascaded = true; }
    }

    // Debug fast-forward: hatch founder brood instantly until target reached
    if (g_ff_target_workers > 0) {
        auto& ch = sim.coordinator.chamber;
        if (ch.conker_count >= (int)g_ff_target_workers) {
            Serial.printf("[ff] target reached: %d workers\r\n", ch.conker_count);
            g_ff_target_workers = 0;
        } else {
            for (int i = 0; i < ch.brood_count; i++) {
                auto& b = ch.brood[i];
                if (b.alive() && b.total_duration_ms > 3) {
                    b.total_duration_ms = 3;
                    b.stage_start_ms = millis() - 10;
                }
            }
        }
    }

    // Satellite: check for OTA cascade from queen
    if (!sim.coordinator.is_queen()) {
        uint32_t queen_fw;
        if (topology_ota_check(&queen_fw) && queen_fw > FW_VERSION) {
            Serial.printf("[ota] Queen has FW v%lu (I have v%lu)\r\n",
                          (unsigned long)queen_fw, (unsigned long)FW_VERSION);
            ota_satellite_update();  // blocking — downloads and reboots
        }
    }

    if (!splashing) {
        handle_serial();
        sim.handle_touch();

        // Hold detection — conkers gather to (and follow) the finger
        TouchEvent hold_ev;
        bool local_hold = touch_holding(&hold_ev);
        if (local_hold) {
            sim.gathering = true;
            sim.gather_is_exit = false;
            sim.gather_x = hold_ev.x / static_cast<float>(Cfg::CELL_SIZE);
            sim.gather_y = hold_ev.y / static_cast<float>(Cfg::CELL_SIZE);
            // Broadcast gather to neighbors so their conkers come too
            for (int f = 0; f < FACE_COUNT; f++) {
                if (sim.coordinator.chamber.entries[f] >= 0) {
                    GatherSyncMessage msg;
                    msg.msg_type  = TOPO_GATHER_SYNC;
                    msg.sender_id = topology_my_id();
                    msg.active    = 1;
                    msg.face      = static_cast<uint8_t>(f);
                    topology_send_to_face(static_cast<Face>(f),
                                          (const uint8_t*)&msg, sizeof(msg));
                }
            }
        } else {
            // Check for remote gather from neighbor
            GatherSyncMessage gmsg;
            if (topology_has_gather(&gmsg)) {
                if (gmsg.active) {
                    sim.gathering = true;
                    sim.gather_is_exit = true;
                    uint8_t local_face = Cfg::FACE_OPPOSITE[gmsg.face];
                    sim.gather_x = static_cast<float>(Cfg::ENTRY_X[local_face]) + 0.5f;
                    sim.gather_y = static_cast<float>(Cfg::ENTRY_Y[local_face]) + 0.5f;
                } else {
                    sim.gathering = false;
                }
            } else if (sim.gathering && !local_hold) {
                // No local hold and no fresh remote message — release
                // (remote sends active=0 on release, or we timeout)
                sim.gathering = false;
            }
            // Broadcast release when local hold ends
            static bool _was_gathering = false;
            if (_was_gathering && !local_hold) {
                for (int f = 0; f < FACE_COUNT; f++) {
                    if (sim.coordinator.chamber.entries[f] >= 0) {
                        GatherSyncMessage msg;
                        msg.msg_type  = TOPO_GATHER_SYNC;
                        msg.sender_id = topology_my_id();
                        msg.active    = 0;
                        msg.face      = static_cast<uint8_t>(f);
                        topology_send_to_face(static_cast<Face>(f),
                                              (const uint8_t*)&msg, sizeof(msg));
                    }
                }
            }
            _was_gathering = local_hold;
        }

        // Sim ticks
        if (g_warp_active) {
            // Time-warp: a batch of fixed-dt ticks with the day/night clock advanced
            // in lockstep — identical to an 8 tps real-time run, just unthrottled.
            // Telemetry runs inside the batch (sim-gated) so it samples every 10
            // sim-seconds; serial/topology/WDT are serviced once per batch (~10 sim-s).
            last_tick_ms = now;  // keep the wall-clock scheduler current for warp-off
            sim.coordinator.chamber.gather_active  = sim.gathering;
            sim.coordinator.chamber.gather_is_exit = sim.gather_is_exit;
            sim.coordinator.chamber.gather_x       = sim.gather_x;
            sim.coordinator.chamber.gather_y       = sim.gather_y;
            for (int i = 0; i < Cfg::WARP_TICKS_PER_LOOP; i++) {
                time_of_day_advance_sim(Cfg::WARP_DT);
                sim.tick(Cfg::WARP_DT);
                g_telemetry.tick(sim.coordinator.chamber, topology_my_id());
                g_telemetry.tick_bonds(sim.coordinator.bonds, sim.coordinator.registry, topology_my_id());
                if (g_tod.unix_time >= g_warp_stop_unix) {
                    g_warp_active = false;
                    last_frame_ms = now;
                    Serial.printf("[warp] target reached — sim clock at local %02d:%02d, warp OFF\r\n",
                                  g_tod.local_hour, g_tod.local_minute);
                    break;
                }
            }
            yield();  // feed the loop-task watchdog between batches
        } else {
            // Run multiple ticks if at high speed
            while (now - last_tick_ms >= interval) {
                last_tick_ms += interval;

                uint32_t sim_now = millis();
                float dt = (sim_now - last_sim_ms) / 1000.0f;
                if (dt > 1.0f) dt = 1.0f;  // cap dt to avoid jumps
                last_sim_ms = sim_now;

                // Wire gather state into chamber before tick
                sim.coordinator.chamber.gather_active  = sim.gathering;
                sim.coordinator.chamber.gather_is_exit = sim.gather_is_exit;
                sim.coordinator.chamber.gather_x       = sim.gather_x;
                sim.coordinator.chamber.gather_y       = sim.gather_y;

                sim.tick(dt);

                // Prevent spiral-of-death
                if (now - last_tick_ms > interval * 4) {
                    last_tick_ms = now;
                    break;
                }
            }
        }
    }

    // Temporary tuning telemetry — sample local conkers' mood/needs to SD
    // (self-times to 10s; runs on both queen and satellite). Under warp this is
    // driven inside the tick batch above (sim-gated), so skip the wall-clock call.
    if (!g_warp_active) {
        g_telemetry.tick(sim.coordinator.chamber, topology_my_id());
        // Parallel bond-strength stream (self-times to 30s).
        g_telemetry.tick_bonds(sim.coordinator.bonds, sim.coordinator.registry, topology_my_id());
    }

    // Render at ~30fps (throttled harder under warp so the SPI flush isn't the bottleneck)
    uint32_t frame_iv = g_warp_active ? Cfg::WARP_RENDER_MS : 33;
    if (now - last_frame_ms >= frame_iv) {
        last_frame_ms = now;

        static Event evt_buf[64];
        int evt_count = 0;
        if (!splashing) {
            evt_count = sim.event_bus.drain(evt_buf, 64);
            renderer.receive_events(evt_buf, evt_count, sim.coordinator.chamber);
            if (sim.coordinator.is_queen() && evt_count > 0)
                sim.coordinator._journal_from_bus_events(evt_buf, evt_count, sim.tick_count);
        }

        float lerp_t = static_cast<float>(now - last_tick_ms) / interval;
        if (lerp_t < 0.0f) lerp_t = 0.0f;
        if (lerp_t > 1.0f) lerp_t = 1.0f;

        renderer.draw(sim.coordinator.chamber, lerp_t);

        // Selection overlay: highlight circle + name above selected conker
        if (sim.selected_conker_id != 0) {
            int sel_idx = -1;
            for (int i = 0; i < sim.coordinator.chamber.conker_count; i++) {
                if (sim.coordinator.chamber.conkers[i].id == sim.selected_conker_id) {
                    sel_idx = i; break;
                }
            }
            if (sel_idx >= 0) {
            Conker& w = sim.coordinator.chamber.conkers[sel_idx];
            if (w.alive) {
                int px = static_cast<int>(w.x * Cfg::CELL_SIZE);
                int py = static_cast<int>(w.y * Cfg::CELL_SIZE);
                int radius = static_cast<int>(w.render_scale() * 6.0f);

                // Highlight circle
                gfx->drawCircle(px, py, radius, 0xFFFF);
                gfx->drawCircle(px, py, radius + 1, 0xFFFF);

                // Name label above head
                const char* name = w.name[0] ? w.name : "???";
                {
                    gfx->setTextSize(1);
                    gfx->setTextWrap(false);
                    int tw = 0;
                    for (const char* p = name; *p; p++) tw += 6;
                    int lx = px - tw / 2;
                    int ly = py - radius - 12;
                    if (ly < 2) ly = 2;
                    // Background pill for readability
                    gfx->fillRoundRect(lx - 3, ly - 1, tw + 6, 11, 3, 0x0000);
                    gfx->setCursor(lx, ly);
                    gfx->setTextColor(0xFFFF);
                    gfx->print(name);
                }

                renderer.mark_dirty_external(px - radius - 5, py - radius - 16,
                                              radius * 2 + 10, radius * 2 + 28);
            } else {
                sim.selected_conker_id = 0;  // died while selected
            }
            } else {
                sim.selected_conker_id = 0;  // no longer in chamber
            }
        }

        if (sim.coordinator.is_queen() && !renderer.is_splash_active()) {
            hud_draw(gfx, sim.coordinator.chamber);
            renderer.mark_dirty_external(0, 0, 480, 28);  // HUD strip
        }
        if (!splashing) {
            hud_draw_battery(gfx);
            hud_draw_version(gfx);
            renderer.mark_dirty_external(0, 304, 480, 16);  // Battery + version strip
        }
        if (topo_overlay) {
            topology_draw_overlay(gfx);
            renderer.mark_dirty_external(0, 0, 480, 320);  // Debug overlay = full screen
        }
        renderer.flush();

        // Log events to serial (skip noisy ones; handoffs logged by coordinator)
        for (int i = 0; i < evt_count; i++) {
            int t = evt_buf[i].type;
            if (t == EVT_INTERACTION_STARTED || t == EVT_INTERACTION_ENDED) continue;
            if (t == EVT_HANDOFF_OUTGOING || t == EVT_HANDOFF_INCOMING) continue;
            if (t >= 0 && t <= 10)
                Serial.printf("[evt t=%6lu] %s\r\n", evt_buf[i].tick, EVT_NAMES[t]);
        }
    }

    // Time of day — once per second
    // Satellite: skip local computation while synced to queen's broadcast
    static unsigned long last_tod = 0;
    static bool tod_synced = false;
    if (now - last_tod >= 1000) {
        last_tod = now;
        bool queen_sync = !sim.coordinator.is_queen() && topology_has_state_sync();
        if (!queen_sync) {
            time_of_day_tick();
            if (tod_synced) {
                Serial.println("[tod] lost queen sync -- using local time");
                tod_synced = false;
            }
        } else if (!tod_synced) {
            Serial.println("[tod] synced to queen time");
            tod_synced = true;
        }
        if (force_daytime) {
            g_tod.phase = PHASE_DAY;
            g_tod.night_factor = 0.0f;
            g_tod.day_progress = 0.5f;
            g_tod.local_hour = 12;
        }
    }

    // WiFi background reconnect (queen only, handles timing internally)
    if (sim.coordinator.is_queen())
        tod_wifi_reconnect_tick();

    // Satellite: solo WiFi sync when no queen connection
    if (!sim.coordinator.is_queen()) {
        bool queen_connected = topology_has_state_sync();
        tod_satellite_wifi_tick(queen_connected);
    }

    // VPS push (queen only, every 30s internally)
    if (sim.coordinator.is_queen())
        vps_push_tick(sim.coordinator);

    // Periodic status
    static unsigned long last_debug = 0;
    if (now - last_debug >= 5000) {
        last_debug = now;
        print_status();
    }
}
