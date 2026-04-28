/*
 * Pin definitions for Waveshare ESP32-S3-Touch-LCD-3.5B
 * Extracted from official Waveshare demo code and schematic.
 *
 * Display: AXS15231B over QSPI
 * LCD reset is routed through a TCA9554 I/O expander (I2C 0x20, pin P1),
 * NOT a direct GPIO — must be toggled via I2C before display init.
 */

#pragma once

// ---- QSPI LCD (AXS15231B) ----
#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK   5
#define LCD_QSPI_D0    1
#define LCD_QSPI_D1    2
#define LCD_QSPI_D2    3
#define LCD_QSPI_D3    4

// ---- LCD backlight (direct GPIO, active HIGH via S8050 transistor) ----
#define LCD_BL          6

// ---- I2C bus (shared: touch, PMU, IO expander, IMU, RTC, codec) ----
#define I2C_SDA         8
#define I2C_SCL         7

// ---- TCA9554 I/O expander ----
#define TCA9554_ADDR    0x20
#define TCA9554_LCD_RST 1   // P1 = LCD_RST

// ---- AXP2101 PMU (not needed for basic display, listed for reference) ----
#define AXP2101_ADDR    0x34

// ---- Display dimensions ----
#define LCD_WIDTH      320
#define LCD_HEIGHT     480

// ---- Topology DETECT pins (reserved, active LOW via neighbour GND) ----
// On J8 header. Not yet wired in main firmware — used by tools/topology-test.
#define DETECT_N       17
#define DETECT_E       18
#define DETECT_S       21
#define DETECT_W       38
