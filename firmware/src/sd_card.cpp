/* SD card mount via SD_MMC 1-bit mode.
 * Follows Waveshare reference demo pattern for ESP32-S3-Touch-LCD-3.5B. */
#include "sd_card.h"
#include "pin_config.h"

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>

void sd_remove_recursive(const char* path, int depth) {
    if (depth > 6) return;  // safety rail
    File dir = SD_MMC.open(path);
    if (!dir) return;
    if (!dir.isDirectory()) {
        dir.close();
        SD_MMC.remove(path);
        return;
    }
    File e;
    while ((e = dir.openNextFile())) {
        String p = String(e.path());
        bool is_dir = e.isDirectory();
        e.close();
        if (is_dir) sd_remove_recursive(p.c_str(), depth + 1);
        else SD_MMC.remove(p);
    }
    dir.close();
    SD_MMC.rmdir(path);
}

static SdState  _state              = SD_NOT_MOUNTED;
static uint8_t  _consecutive_fails  = 0;
static uint32_t _error_enter_ms     = 0;

static constexpr uint8_t  FAIL_THRESHOLD    = 3;
static constexpr uint32_t RETRY_INTERVAL_MS = 60000;  // 60s before remount attempt

// ---- TCA9554 helpers (Wire must already be initialised) ----

static void _tca_set_p3_high() {
    // Set P3 as output
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);  // config register
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    uint8_t cfg = Wire.read();
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);
    Wire.write(cfg & ~(1 << TCA9554_SD_CS));  // 0 = output
    Wire.endTransmission();

    // Drive P3 HIGH (DAT3 pull-up — keeps card in SD mode)
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);  // output register
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    uint8_t out = Wire.read();
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);
    Wire.write(out | (1 << TCA9554_SD_CS));
    Wire.endTransmission();
}

// ---- Public API ----

bool sd_card_init() {
    _tca_set_p3_high();

    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
        Serial.println("[sd] mount failed (SD_MMC 1-bit)");

        // Try again with P3 LOW in case board expects SPI-style CS
        Wire.beginTransmission(TCA9554_ADDR);
        Wire.write(0x01);
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
        uint8_t out = Wire.read();
        Wire.beginTransmission(TCA9554_ADDR);
        Wire.write(0x01);
        Wire.write(out & ~(1 << TCA9554_SD_CS));
        Wire.endTransmission();

        if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
            Serial.println("[sd] mount failed (retry with CS low)");
            _state = SD_NOT_MOUNTED;
            return false;
        }
    }

    uint8_t ct = SD_MMC.cardType();
    if (ct == CARD_NONE) {
        Serial.println("[sd] no card detected");
        _state = SD_NOT_MOUNTED;
        return false;
    }

    const char* type_str = "UNKNOWN";
    if (ct == CARD_MMC)  type_str = "MMC";
    if (ct == CARD_SD)   type_str = "SD";
    if (ct == CARD_SDHC) type_str = "SDHC";

    uint64_t total = SD_MMC.totalBytes();
    uint64_t used  = SD_MMC.usedBytes();
    Serial.printf("[sd] mounted OK — %s, %.1f MB total, %.1f MB used\r\n",
                  type_str,
                  total / (1024.0 * 1024.0),
                  used  / (1024.0 * 1024.0));

    _state = SD_OK;
    _consecutive_fails = 0;
    return true;
}

SdState sd_card_state() { return _state; }

uint64_t sd_card_total_bytes() {
    return (_state == SD_OK) ? SD_MMC.totalBytes() : 0;
}

uint64_t sd_card_used_bytes() {
    return (_state == SD_OK) ? SD_MMC.usedBytes() : 0;
}

void sd_card_write_failed() {
    _consecutive_fails++;
    if (_consecutive_fails >= FAIL_THRESHOLD && _state == SD_OK) {
        Serial.printf("[sd] %d consecutive write failures — entering ERROR state\r\n",
                      _consecutive_fails);
        _state = SD_ERROR;
        _error_enter_ms = millis();
    }
}

void sd_card_write_ok() {
    _consecutive_fails = 0;
    if (_state == SD_ERROR) {
        Serial.println("[sd] write succeeded — back to OK");
        _state = SD_OK;
    }
}

void sd_card_health_tick() {
    if (_state != SD_ERROR) return;
    if (millis() - _error_enter_ms < RETRY_INTERVAL_MS) return;
    Serial.println("[sd] attempting remount...");
    sd_card_remount();
}

bool sd_card_remount() {
    SD_MMC.end();
    delay(100);
    bool ok = sd_card_init();
    if (!ok) {
        _state = SD_ERROR;
        _error_enter_ms = millis();
    }
    return ok;
}
