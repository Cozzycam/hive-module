/* Touch input abstraction.
 *
 * touch_poll() returns true when a tap event is available. The
 * caller converts display coordinates to chamber grid cells and
 * feeds the result into the sim via chamber.add_food().
 *
 * On the Waveshare ESP32-S3-Touch-LCD-3.5B the touch controller is
 * the same AXS15231B IC used for the display, accessed over I2C.
 * Until the hardware driver is filled in, touch_poll() returns false.
 */
#pragma once

#include <cstdint>

struct TouchEvent {
    int16_t x;   // display pixel x (0..479 after rotation)
    int16_t y;   // display pixel y (0..319 after rotation)
};

/* Initialise the touch controller. Call once in setup(). */
void touch_init();

/* Poll for a tap event (press-release on a single point, not drag).
 * Returns true and fills *out if an event is available. */
bool touch_poll(TouchEvent* out);
