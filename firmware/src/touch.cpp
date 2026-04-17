/* Touch input — AXS15231B capacitive touch over I2C.
 *
 * The AXS15231B on the Waveshare ESP32-S3-Touch-LCD-3.5B shares the
 * I2C bus (SDA=8, SCL=7) with the TCA9554 IO expander. The touch
 * controller I2C address and register map need to be confirmed from
 * the Waveshare datasheet / demo code.
 *
 * Tap detection: track press→release. If the touch point didn't move
 * more than a few pixels between press and release, it's a tap.
 */

#include "touch.h"

// TODO: AXS15231B I2C touch controller address and registers.
// static constexpr uint8_t TOUCH_I2C_ADDR = 0x3B;  // confirm from datasheet

void touch_init() {
    // TODO: AXS15231B touch controller I2C initialisation.
    // Wire is already started in main.cpp (SDA=8, SCL=7).
    // - Read chip ID register to confirm touch IC is present
    // - Set operating mode (normal / low-power)
    // - Configure interrupt pin if available
}

bool touch_poll(TouchEvent* out) {
    // TODO: AXS15231B I2C read — check for touch data.
    //
    // Pseudocode for when hardware is available:
    //   1. Read touch status register
    //   2. If no touch, return false
    //   3. If touch down and wasn't down last poll → record press position
    //   4. If touch up and was down last poll:
    //      a. Compute distance from press position
    //      b. If distance < TAP_THRESHOLD (~10px), it's a tap:
    //         out->x = press_x;  out->y = press_y;
    //         return true;
    //   5. Return false otherwise (still touching, or drag)
    //
    (void)out;
    return false;
}
