/* Status HUD — thin horizontal strip overlay (28px) at top of display.
 *
 * Shows: [pop icon] N LilGuys · Day X · [food icon] Y days food ... phase [pulse dot]
 * Palette re-tints with day/dusk/night cycle.
 * Reads ColonyState, Chamber, g_tod. Smooth number animation (~200ms lerp).
 */
#pragma once
#include <Arduino_GFX_Library.h>
#include "chamber.h"

void hud_init();
void hud_draw(Arduino_Canvas* gfx, const Chamber& ch);

// Battery indicator — works on both queen and satellite.
// Call hud_battery_init() once in setup (after Wire.begin).
// Call hud_draw_battery() every frame.
void hud_battery_init();
void hud_draw_battery(Arduino_Canvas* gfx);
