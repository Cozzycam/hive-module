/* Touch input — AXS15231B integrated capacitive touch over I2C.
 *
 * touch_poll() returns true when a tap event is available. The
 * caller converts display coordinates to chamber grid cells and
 * feeds the result into the sim via chamber.add_food().
 *
 * On the Waveshare ESP32-S3-Touch-LCD-3.5B the touch controller is
 * the same AXS15231B IC used for the display, accessed over I2C
 * at address 0x3B. Touch coordinates arrive in native (portrait)
 * orientation and are rotated here to match setRotation(1).
 */
#pragma once

#include <cstdint>

struct TouchEvent {
    int16_t x;   // display pixel x (0..479 after rotation)
    int16_t y;   // display pixel y (0..319 after rotation)
};

/* Initialise the touch controller. Call once in setup(),
 * after Wire.begin() and the TCA9554 LCD reset sequence. */
void touch_init();

/* Poll for a tap event (press-release on a single point, not drag).
 * Returns true and fills *out if an event is available. */
bool touch_poll(TouchEvent* out);

/* Returns true if a finger is currently held down (>300ms).
 * Fills *out with the current rotated display coordinates.
 * Suppressed while a swipe is in progress (drag that began moving
 * before the hold threshold). */
bool touch_holding(TouchEvent* out);

/* Poll for a completed swipe — a drag that started moving within the
 * hold threshold and has now lifted. Fills out with up to max sampled
 * path points (rotated display coords). Returns point count, 0 if none. */
int touch_swipe(TouchEvent* out, int max_points);
