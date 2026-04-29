/* OTA cascade implementation. */
#include "ota_push.h"
#include "config.h"
#include "time_of_day.h"
#include "topology.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>

// ================================================================
//  Queen: push firmware to satellites
// ================================================================

void ota_push() {
    size_t fw_size = ESP.getSketchSize();
    Serial.printf("[push] FW_VERSION=%lu, image=%u bytes\n",
                  (unsigned long)FW_VERSION, fw_size);

    // Phase 1: Broadcast OTA announce on ESP-NOW (channel 1).
    // Satellites hear this and prepare to connect to WiFi.
    OtaAnnounceMessage announce;
    announce.msg_type   = TOPO_OTA_ANNOUNCE;
    announce.sender_id  = topology_my_id();
    announce.fw_version = FW_VERSION;

    Serial.println("[push] Broadcasting announce (5x over 2s)...");
    for (int i = 0; i < 5; i++) {
        topology_broadcast((const uint8_t*)&announce, sizeof(announce));
        delay(400);
    }

    // Phase 2: Connect to home WiFi
    Serial.println("[push] Connecting to WiFi...");
    if (!tod_wifi_connect(15000)) {
        Serial.println("[push] WiFi failed — rebooting");
        delay(100);
        ESP.restart();
    }

    // Phase 3: Start HTTP server serving our running firmware partition
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        Serial.println("[push] Can't read running partition — rebooting");
        delay(100);
        ESP.restart();
    }

    WiFiServer httpServer(8266);
    httpServer.begin();

    // Prepare ready message with our IP
    OtaReadyMessage ready;
    ready.msg_type   = TOPO_OTA_READY;
    ready.sender_id  = topology_my_id();
    ready.fw_version = FW_VERSION;
    ready.port       = 8266;
    IPAddress ip = WiFi.localIP();
    ready.ip[0] = ip[0]; ready.ip[1] = ip[1];
    ready.ip[2] = ip[2]; ready.ip[3] = ip[3];

    // Count connected satellites so we know when we're done
    int expected = 0;
    for (int f = 0; f < FACE_COUNT; f++)
        if (topology_neighbour((Face)f).present) expected++;

    Serial.printf("[push] Server at %s:8266 — expecting %d satellite(s) (2 min, 'q' cancel)\n",
                  ip.toString().c_str(), expected);

    uint32_t start = millis();
    uint32_t last_bcast = 0;
    int served = 0;

    while (millis() - start < 120000) {
        // Broadcast server address every 2s (satellites on WiFi channel can hear this)
        if (millis() - last_bcast > 2000) {
            last_bcast = millis();
            topology_broadcast((const uint8_t*)&ready, sizeof(ready));
        }

        // Serve firmware to connecting clients
        WiFiClient client = httpServer.available();
        if (client) {
            Serial.println("[push] Satellite connected — sending firmware...");

            // Consume HTTP request headers
            uint32_t t0 = millis();
            while (client.connected() && millis() - t0 < 5000) {
                if (!client.available()) { delay(1); continue; }
                String line = client.readStringUntil('\n');
                if (line.length() <= 1) break;
            }

            // Respond with firmware binary
            client.printf("HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/octet-stream\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n\r\n", fw_size);

            uint8_t buf[4096];
            size_t sent = 0;
            while (sent < fw_size && client.connected()) {
                size_t chunk = (fw_size - sent > sizeof(buf)) ? sizeof(buf) : fw_size - sent;
                esp_partition_read(running, sent, buf, chunk);
                client.write(buf, chunk);
                sent += chunk;
            }
            client.stop();
            served++;
            Serial.printf("[push] Sent %u bytes (%d/%d satellite(s) done)\n", sent, served, expected);
            if (served >= expected && expected > 0) break;
        }

        if (Serial.available() && Serial.read() == 'q') {
            Serial.println("[push] Cancelled");
            break;
        }
        delay(10);
    }

    Serial.printf("[push] Served %d satellite(s) — rebooting\n", served);
    delay(100);
    ESP.restart();
}

// ================================================================
//  Satellite: receive firmware from queen
// ================================================================

void ota_satellite_update() {
    Serial.printf("[ota-recv] Updating from FW v%lu...\n", (unsigned long)FW_VERSION);

    // Connect to home WiFi (same AP as queen = same channel)
    if (!tod_wifi_connect(15000)) {
        Serial.println("[ota-recv] WiFi failed — rebooting");
        delay(100);
        ESP.restart();
    }

    // Wait for queen's TOPO_OTA_READY with server IP
    Serial.println("[ota-recv] Waiting for queen's server address...");
    OtaReadyMessage ready;
    uint32_t start = millis();

    while (millis() - start < 60000) {
        if (topology_ota_server(&ready)) break;
        delay(100);
    }

    if (ready.ip[0] == 0 && ready.ip[1] == 0 && ready.ip[2] == 0 && ready.ip[3] == 0) {
        Serial.println("[ota-recv] No server address received — rebooting");
        delay(100);
        ESP.restart();
    }

    // Download and flash
    char url[64];
    snprintf(url, sizeof(url), "http://%d.%d.%d.%d:%d/firmware.bin",
             ready.ip[0], ready.ip[1], ready.ip[2], ready.ip[3], ready.port);
    Serial.printf("[ota-recv] Downloading %s\n", url);

    WiFiClient client;
    httpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    // Only reached on failure (success reboots automatically)
    Serial.printf("[ota-recv] Failed: %s — rebooting\n",
                  httpUpdate.getLastErrorString().c_str());
    delay(100);
    ESP.restart();
}
