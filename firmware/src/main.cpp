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
#include "hud.h"
#include "topology.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "ota_push.h"

// Event type names for serial logging
static const char* EVT_NAMES[] = {
    "interaction_started", "interaction_ended",
    "food_delivered", "food_tapped", "pile_discovered",
    "queen_laid_egg", "young_hatched", "young_died",
    "lil_guy_died", "handoff_incoming", "handoff_outgoing",
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
Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, -1, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// -- Sim + Renderer --------------------------------------------------

static Sim sim;
static Renderer renderer;
static bool topo_overlay = false;  // debug overlay toggle

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

// -- Timing ----------------------------------------------------------

static unsigned long last_tick_ms = 0;
static unsigned long last_frame_ms = 0;

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
        Serial.printf("[ota] Error[%u] — rebooting to restore topology\n", error);
        delay(100);
        ESP.restart();
    });
    ArduinoOTA.begin();

    Serial.printf("[ota] Ready. Upload with:\n");
    Serial.printf("  pio run -t upload --upload-port %s\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[ota] Waiting 2 min... press 'q' to cancel (will reboot)\n");
    Serial.printf("[ota] FW_VERSION=%lu\n", (unsigned long)FW_VERSION);

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
        "Pressure %.2f | Speed %d tps\n",
        sim.coordinator.colony.population, sim.coordinator.colony.food_total,
        sim.coordinator.colony.brood_egg, sim.coordinator.colony.brood_larva, sim.coordinator.colony.brood_pupa,
        sim.coordinator.colony.food_pressure(),
        SPEED_LEVELS[speed_index]);
}

// Line buffer for multi-word serial commands (e.g. "role queen")
static char serial_buf[64];
static int  serial_len = 0;

static void process_serial_line(const char* line) {
    // Multi-word commands
    if (strncmp(line, "role queen", 10) == 0) {
        Coordinator::set_role_nvs(MODULE_QUEEN);
        Serial.println("Rebooting as queen...");
        delay(100);
        ESP.restart();
    } else if (strncmp(line, "role satellite", 14) == 0) {
        Coordinator::set_role_nvs(MODULE_SATELLITE);
        Serial.println("Rebooting as satellite...");
        delay(100);
        ESP.restart();
    } else if (strcmp(line, "ota") == 0) {
        enter_ota_mode();
    } else if (strcmp(line, "push") == 0) {
        ota_push();
    } else if (strcmp(line, "topology") == 0) {
        sim.coordinator.print_topology();
    } else if (strlen(line) == 1) {
        // Single-char commands
        char c = line[0];
        switch (c) {
        case '+': case 'f':
            if (speed_index < NUM_SPEEDS - 1) speed_index++;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '-': case 's':
            if (speed_index > 0) speed_index--;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
        case '9':
            speed_index = c - '1';
            if (speed_index >= NUM_SPEEDS) speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '0':
            speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
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
            Serial.printf("Topology overlay: %s\n", topo_overlay ? "ON" : "OFF");
            if (!topo_overlay) renderer.force_full_redraw();
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

    touch_init();
    time_of_day_init();
    sim.init();
    topology_init(topo_callback);  // after WiFi/NTP, before renderer
    renderer.init(gfx);

    hud_battery_init();  // AXP2101 probe — works on both queen and satellite

    bool queen = sim.coordinator.is_queen();
    if (queen) {
        hud_init();
        renderer.start_boot_splash();
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

    // Satellite: check for OTA cascade from queen
    if (!sim.coordinator.is_queen()) {
        uint32_t queen_fw;
        if (topology_ota_check(&queen_fw) && queen_fw > FW_VERSION) {
            Serial.printf("[ota] Queen has FW v%lu (I have v%lu)\n",
                          (unsigned long)queen_fw, (unsigned long)FW_VERSION);
            ota_satellite_update();  // blocking — downloads and reboots
        }
    }

    if (!splashing) {
        handle_serial();
        sim.handle_touch();

        // Sim ticks — run multiple if at high speed
        while (now - last_tick_ms >= interval) {
            last_tick_ms += interval;

            uint32_t sim_now = millis();
            float dt = (sim_now - last_sim_ms) / 1000.0f;
            if (dt > 1.0f) dt = 1.0f;  // cap dt to avoid jumps
            last_sim_ms = sim_now;

            sim.tick(dt);

            // Prevent spiral-of-death
            if (now - last_tick_ms > interval * 4) {
                last_tick_ms = now;
                break;
            }
        }
    }

    // Render at ~30fps
    if (now - last_frame_ms >= 33) {
        last_frame_ms = now;

        static Event evt_buf[64];
        int evt_count = 0;
        if (!splashing) {
            evt_count = sim.event_bus.drain(evt_buf, 64);
            renderer.receive_events(evt_buf, evt_count, sim.coordinator.chamber);
        }

        float lerp_t = static_cast<float>(now - last_tick_ms) / interval;
        if (lerp_t < 0.0f) lerp_t = 0.0f;
        if (lerp_t > 1.0f) lerp_t = 1.0f;

        renderer.draw(sim.coordinator.chamber, lerp_t);
        if (sim.coordinator.is_queen() && !renderer.is_splash_active())
            hud_draw(gfx, sim.coordinator.chamber);
        if (!splashing)
            hud_draw_battery(gfx);
        if (topo_overlay) topology_draw_overlay(gfx);
        renderer.flush();

        // Log events to serial (skip noisy ones; handoffs logged by coordinator)
        for (int i = 0; i < evt_count; i++) {
            int t = evt_buf[i].type;
            if (t == EVT_INTERACTION_STARTED || t == EVT_INTERACTION_ENDED) continue;
            if (t == EVT_HANDOFF_OUTGOING || t == EVT_HANDOFF_INCOMING) continue;
            if (t >= 0 && t <= 10)
                Serial.printf("[evt t=%6lu] %s\n", evt_buf[i].tick, EVT_NAMES[t]);
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
    }

    // Periodic status
    static unsigned long last_debug = 0;
    if (now - last_debug >= 5000) {
        last_debug = now;
        print_status();
    }
}
