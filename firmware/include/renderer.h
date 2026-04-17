/* Chamber renderer — 1:1 full-chamber rendering.
 * Grid is 30×20 at CELL_SIZE=16 = 480×320 = full screen.
 * Uses dirty-rect tracking to minimize PSRAM writes.
 */
#pragma once

#include <Arduino_GFX_Library.h>
#include "chamber.h"

class Renderer {
public:
    void init(Arduino_Canvas* canvas);
    void draw(const Chamber& ch, float lerp_t);
    void flush();
    void force_full_redraw() { _needs_full_redraw = true; }

private:
    Arduino_Canvas* _gfx = nullptr;
    bool _needs_full_redraw = true;

    struct DirtyRect { int16_t x, y, w, h; };
    static constexpr int MAX_DIRTY = 256;
    DirtyRect _dirty[MAX_DIRTY];
    int _dirty_count = 0;

    void _mark_dirty(int sx, int sy, int sw, int sh);
    void _clear_dirty();

    void _draw_background_full(const Chamber& ch);
    void _draw_food_piles(const Chamber& ch);
    void _draw_brood(const Chamber& ch);
    void _draw_queen(const Chamber& ch);
    void _draw_workers(const Chamber& ch, float lerp_t);
    void _draw_sprite(int cx, int cy, const uint16_t* data, int sw, int sh);
};
