/* Touch input — AXS15231B capacitive touch over I2C.
 *
 * The AXS15231B on the Waveshare ESP32-S3-Touch-LCD-3.5B has an
 * integrated touch controller on the same I2C bus (SDA=8, SCL=7)
 * as the TCA9554 IO expander. Touch address is 0x3B.
 *
 * Protocol: write an 11-byte command, read 14 bytes back.
 * Coordinates come in native portrait orientation (x: 0-319,
 * y: 0-479). We rotate to match setRotation(1) before returning.
 *
 * Tap detection: track press->release. If the touch point didn't
 * move more than TAP_THRESHOLD pixels between press and release,
 * it's a tap.
 */

#include "touch.h"
#include <Wire.h>

static constexpr uint8_t TOUCH_I2C_ADDR = 0x3B;

// 11-byte read command — requests 14 bytes of touch data
static const uint8_t TOUCH_CMD[11] = {
    0xB5, 0xAB, 0xA5, 0x5A,
    0x00, 0x00, 0x00, 0x0E,
    0x00, 0x00, 0x00
};

// Tap detection thresholds
static constexpr int TAP_THRESHOLD = 15;   // max pixel drift for a tap

// TCA9554 touch reset pin
static constexpr uint8_t TCA9554_ADDR      = 0x20;
static constexpr uint8_t TCA9554_TOUCH_RST  = 3;   // P3 = touch reset

// Internal state for press/release tracking
static bool     _was_touching = false;
static int16_t  _press_x = 0;    // native coords at press
static int16_t  _press_y = 0;

// Read raw touch data. Returns true if a finger is currently down,
// and fills native_x/native_y with portrait-orientation coords.
static bool _read_touch(int16_t& native_x, int16_t& native_y) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(TOUCH_CMD, sizeof(TOUCH_CMD));
    if (Wire.endTransmission() != 0)
        return false;

    uint8_t data[14];
    uint8_t got = Wire.requestFrom(TOUCH_I2C_ADDR, (uint8_t)14);
    if (got < 14)
        return false;
    for (int i = 0; i < 14; i++)
        data[i] = Wire.read();

    // Validate
    if (data[0] == 0xFF)       return false;   // no valid data
    if (data[1] == 0 || data[1] > 2) return false;   // no touch or invalid

    // Extract 12-bit coordinates (lower 4 bits of high byte + low byte)
    int16_t x = ((data[2] & 0x0F) << 8) | data[3];
    int16_t y = ((data[4] & 0x0F) << 8) | data[5];

    // Reject obviously spurious readings
    if (x == 0 && y < 2) return false;

    native_x = x;
    native_y = y;
    return true;
}

void touch_init() {
    // Hardware reset the touch controller via TCA9554 P3.
    // Wire is already started in main.cpp (SDA=8, SCL=7).
    // Read current config, set P3 as output.
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);  // config register
    Wire.endTransmission(false);
    Wire.requestFrom(TCA9554_ADDR, (uint8_t)1);
    uint8_t cfg = Wire.read();

    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);
    Wire.write(cfg & ~(1 << TCA9554_TOUCH_RST));  // P3 as output
    Wire.endTransmission();

    // Toggle reset: HIGH -> LOW -> HIGH
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);  // output register
    Wire.endTransmission(false);
    Wire.requestFrom(TCA9554_ADDR, (uint8_t)1);
    uint8_t out = Wire.read();

    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);
    Wire.write(out | (1 << TCA9554_TOUCH_RST));
    Wire.endTransmission();
    delay(10);

    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);
    Wire.write(out & ~(1 << TCA9554_TOUCH_RST));
    Wire.endTransmission();
    delay(10);

    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);
    Wire.write(out | (1 << TCA9554_TOUCH_RST));
    Wire.endTransmission();
    delay(200);

    _was_touching = false;
}

bool touch_poll(TouchEvent* out) {
    int16_t nx, ny;
    bool touching = _read_touch(nx, ny);

    if (touching && !_was_touching) {
        // Finger just went down — record press position
        _press_x = nx;
        _press_y = ny;
        _was_touching = true;
        return false;
    }

    if (!touching && _was_touching) {
        // Finger just lifted
        _was_touching = false;

        // If press was cancelled by drag, don't emit
        if (_press_x < 0) return false;

        // Rotate from native portrait to landscape (setRotation(1)):
        //   display_x = native_y       (0..479)
        //   display_y = 319 - native_x (0..319)
        out->x = _press_y;
        out->y = 319 - _press_x;
        return true;
    }

    if (touching && _was_touching) {
        // Still touching — check for drag to cancel tap
        int dx = nx - _press_x;
        int dy = ny - _press_y;
        if (dx * dx + dy * dy > TAP_THRESHOLD * TAP_THRESHOLD) {
            // Moved too far — this is a drag, not a tap
            _press_x = -1000;
            _press_y = -1000;
        }
    }

    _was_touching = touching;
    return false;
}
