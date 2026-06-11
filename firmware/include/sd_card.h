/* SD card mount/unmount via SD_MMC 1-bit mode.
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-3.5B
 *   CLK = GPIO 11, CMD = GPIO 10, D0 = GPIO 9
 *   DAT3/CS routed through TCA9554 P3 (I2C expander).
 *   Set P3 HIGH at boot to keep card in SD mode (not SPI).
 *
 * SD_MMC in 1-bit mode is Waveshare's reference approach for this board.
 */
#pragma once
#include <cstdint>

enum SdState : uint8_t {
    SD_NOT_MOUNTED = 0,
    SD_OK          = 1,
    SD_ERROR       = 2,   // consecutive write failures, waiting to retry
};

// Mount the SD card.  Call once during setup(), after Wire.begin().
// Returns true if mount succeeded.
bool sd_card_init();

// Current mount state.
SdState sd_card_state();

// Recursively delete a path (files + nested dirs, capped depth).
void sd_remove_recursive(const char* path, int depth = 0);

// Total / used size in bytes (0 if not mounted).
uint64_t sd_card_total_bytes();
uint64_t sd_card_used_bytes();

// Health check — call periodically.  If in ERROR state and enough time
// has elapsed, attempts a remount.
void sd_card_health_tick();

// Record a write failure (called by persistence layer).
// After N consecutive failures, transitions to SD_ERROR.
void sd_card_write_failed();

// Record a successful write (resets failure counter).
void sd_card_write_ok();

// Attempt remount (e.g. after card reinserted).  Returns true on success.
bool sd_card_remount();
