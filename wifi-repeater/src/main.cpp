/*
 * WiFi NAT Repeater for ESP32 (Arduino-ESP32 v3.x API)
 *
 * Connects to upstream router as STA, creates a local AP,
 * and forwards traffic using built-in NAPT.
 * Includes ArduinoOTA for remote firmware updates.
 *
 * Status LED (pin 13 on HUZZAH32):
 *   Blinking = trying to connect to upstream
 *   Solid    = connected and repeating
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

// ---- Upstream (your router) ----
static const char* STA_SSID = "EE-52GKNR";
static const char* STA_PASS = "F7QMktKEtvP6MqFH";

// ---- Local AP (what the queen connects to) ----
static const char* AP_SSID  = "HiveRelay";
static const char* AP_PASS  = "hiverelay123";

static const int LED_PIN = 13;
static bool napt_started = false;
static bool ota_started = false;

// ---- Persistent log ----
static Preferences prefs;
static const int LOG_MAX = 2048;

static void plog(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.println(buf);

    prefs.begin("log", false);
    String log = prefs.getString("ring", "");
    log += buf;
    log += "\n";
    if (log.length() > LOG_MAX)
        log = log.substring(log.length() - LOG_MAX);
    prefs.putString("ring", log);
    prefs.end();
}

static void print_and_clear_log() {
    prefs.begin("log", false);
    String log = prefs.getString("ring", "");
    if (log.length() > 0) {
        Serial.println("=== SAVED LOG ===");
        Serial.print(log);
        Serial.println("=== END ===");
        prefs.putString("ring", "");
    }
    prefs.end();
}

static const char* reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "PANIC/CRASH";
        case ESP_RST_INT_WDT:  return "INT_WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
        case ESP_RST_WDT:      return "OTHER_WATCHDOG";
        case ESP_RST_BROWNOUT: return "brownout";
        default:               return "unknown";
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.printf("\n[repeater] WiFi NAT Repeater (reset: %s, heap: %lu)\n",
        reset_reason_str(), (unsigned long)ESP.getFreeHeap());
    print_and_clear_log();

    pinMode(LED_PIN, OUTPUT);
    plog("--- BOOT reset=%s heap=%lu ---", reset_reason_str(), (unsigned long)ESP.getFreeHeap());

    // v3.x API: AP + STA mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);

    // Configure AP with DNS set to 8.8.8.8 (so clients route DNS through NAPT)
    WiFi.AP.begin();
    WiFi.AP.config(
        IPAddress(192, 168, 4, 1),    // local IP
        IPAddress(192, 168, 4, 1),    // gateway
        IPAddress(255, 255, 255, 0),  // subnet
        IPAddress(192, 168, 4, 2),    // DHCP lease start
        IPAddress(8, 8, 8, 8)         // DNS server for clients
    );
    WiFi.AP.create(AP_SSID, AP_PASS);
    plog("[boot] AP '%s' on 192.168.4.1 (DNS: 8.8.8.8)", AP_SSID);

    // Connect to upstream
    WiFi.begin(STA_SSID, STA_PASS);
    plog("[boot] connecting to '%s'", STA_SSID);
}

void loop() {
    static uint32_t last_blink = 0;
    static bool led_state = false;
    static uint32_t last_status = 0;
    static uint8_t prev_clients = 0;

    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);

        if (!napt_started) {
            plog("[napt] STA IP=%s ch=%d heap=%lu",
                WiFi.localIP().toString().c_str(), WiFi.channel(),
                (unsigned long)ESP.getFreeHeap());
            WiFi.AP.enableNAPT(true);
            napt_started = true;
            plog("[napt] enabled, relay ready");
        }

        if (!ota_started) {
            ArduinoOTA.setHostname("hive-relay");
            ArduinoOTA.begin();
            ota_started = true;
            plog("[ota] ready");
        }

        ArduinoOTA.handle();

        uint8_t clients = WiFi.softAPgetStationNum();
        if (clients != prev_clients) {
            plog("[ap] clients: %d -> %d", prev_clients, clients);
            prev_clients = clients;
        }

        if (millis() - last_status > 30000) {
            plog("[status] heap=%lu clients=%d up=%lus",
                (unsigned long)ESP.getFreeHeap(), clients,
                (unsigned long)(millis() / 1000));
            last_status = millis();
        }
    } else {
        if (napt_started) {
            WiFi.AP.enableNAPT(false);
            plog("[sta] disconnected, NAPT off");
        }
        napt_started = false;
        ota_started = false;

        if (millis() - last_blink > 500) {
            led_state = !led_state;
            digitalWrite(LED_PIN, led_state);
            last_blink = millis();
        }

        static uint32_t last_retry = 0;
        if (millis() - last_retry > 15000) {
            plog("[sta] retrying '%s'", STA_SSID);
            esp_wifi_disconnect();
            delay(100);
            WiFi.begin(STA_SSID, STA_PASS);
            last_retry = millis();
        }
    }

    delay(10);
}
