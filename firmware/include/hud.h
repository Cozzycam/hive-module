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
