/* Chamber renderer — 1:1 full-chamber, dirty-rect optimized.
 *
 * With CELL_SIZE=16 and grid 30x20, the full chamber fills 480x320
 * exactly. No viewport or scaling needed. Screen borders = chamber borders.
 */
#include "renderer.h"
#include "palette.h"
#include "sprites.h"
#include <pgmspace.h>

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;

void Renderer::init(Arduino_Canvas* canvas) {
    _gfx = canvas;
    _needs_full_redraw = true;
    _dirty_count = 0;
}

void Renderer::flush() { _gfx->flush(); }

// -- Dirty rect management -------------------------------------------

void Renderer::_mark_dirty(int sx, int sy, int sw, int sh) {
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > SCREEN_W) sw = SCREEN_W - sx;
    if (sy + sh > SCREEN_H) sh = SCREEN_H - sy;
    if (sw <= 0 || sh <= 0) return;
    if (_dirty_count < MAX_DIRTY)
        _dirty[_dirty_count++] = {
            static_cast<int16_t>(sx), static_cast<int16_t>(sy),
            static_cast<int16_t>(sw), static_cast<int16_t>(sh)};
}

void Renderer::_clear_dirty() {
    for (int i = 0; i < _dirty_count; i++) {
        auto& d = _dirty[i];
        _gfx->fillRect(d.x, d.y, d.w, d.h, PAL_SOIL_DARK);
    }
    _dirty_count = 0;
}

// -- Main draw -------------------------------------------------------

void Renderer::draw(const Chamber& ch, float lerp_t) {
    if (_needs_full_redraw) {
        _gfx->fillScreen(PAL_SOIL_DARK);
        _dirty_count = 0;
        _needs_full_redraw = false;
    } else {
        _clear_dirty();
    }

    // Redraw glow every frame — cheap (3 fillRects) and covers
    // dirty rect clearing so transparent sprite pixels show glow,
    // not the SOIL_DARK fill from _clear_dirty.
    _draw_background_full(ch);

    _draw_food_piles(ch);
    _draw_brood(ch);
    if (ch.has_queen) _draw_queen(ch);
    _draw_workers(ch, lerp_t);
}

// -- Background (full redraw only) -----------------------------------

void Renderer::_draw_background_full(const Chamber& ch) {
    if (ch.has_queen && ch.queen_obj.alive) {
        int qpx = ch.queen_obj.x * Cfg::CELL_SIZE;
        int qpy = ch.queen_obj.y * Cfg::CELL_SIZE;
        struct { uint16_t col; int r; } rings[] = {
            { PAL_SOIL_MID,   200 },
            { PAL_SOIL_LIGHT, 120 },
            { PAL_GLOW_WARM,  60 },
        };
        for (auto& ring : rings) {
            int rx = qpx - ring.r;
            int ry = qpy - ring.r;
            int rw = ring.r * 2;
            int rh = ring.r * 2;
            if (rx < 0) { rw += rx; rx = 0; }
            if (ry < 0) { rh += ry; ry = 0; }
            if (rx + rw > SCREEN_W) rw = SCREEN_W - rx;
            if (ry + rh > SCREEN_H) rh = SCREEN_H - ry;
            if (rw > 0 && rh > 0)
                _gfx->fillRect(rx, ry, rw, rh, ring.col);
        }
    }
}

// -- Food piles ------------------------------------------------------

void Renderer::_draw_food_piles(const Chamber& ch) {
    for (int i = 0; i < ch.food_pile_count; i++) {
        int cx = ch.food_piles[i].x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int cy = ch.food_piles[i].y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        float amt = ch.food_piles[i].amount;
        if (amt <= 0) continue;

        _mark_dirty(cx - 6, cy - 4, 12, 8);

        if (amt <= 15) {
            _gfx->fillRect(cx - 2, cy - 1, 4, 3, PAL_FOOD_LIGHT);
        } else if (amt <= 50) {
            _gfx->fillRect(cx - 4, cy - 2, 4, 3, PAL_FOOD_DARK);
            _gfx->fillRect(cx,     cy,     4, 3, PAL_FOOD_LIGHT);
        } else {
            _gfx->fillRect(cx - 4, cy - 2, 4, 3, PAL_FOOD_DARK);
            _gfx->fillRect(cx,     cy,     4, 3, PAL_FOOD_LIGHT);
            _gfx->fillRect(cx + 2, cy - 2, 4, 3, PAL_FOOD_DARK);
            _gfx->fillRect(cx - 2, cy + 2, 4, 3, PAL_FOOD_LIGHT);
        }
    }
}

// -- Sprites (1:1, no scaling) ---------------------------------------

void Renderer::_draw_sprite(int cx, int cy, const uint16_t* data, int sw, int sh) {
    int ox = cx - sw / 2;
    int oy = cy - sh / 2;

    _mark_dirty(ox, oy, sw, sh);

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint16_t c = pgm_read_word(&data[y * sw + x]);
            if (c == SPRITE_TRANSPARENT) continue;
            int px = ox + x;
            int py = oy + y;
            if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                _gfx->drawPixel(px, py, c);
        }
    }
}

void Renderer::_draw_brood(const Chamber& ch) {
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (!b.alive()) continue;
        int px = b.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int py = b.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        switch (b.stage) {
            case STAGE_EGG:   _draw_sprite(px, py, EGG,   EGG_W,   EGG_H);   break;
            case STAGE_LARVA: _draw_sprite(px, py, LARVA, LARVA_W, LARVA_H); break;
            case STAGE_PUPA:  _draw_sprite(px, py, PUPA,  PUPA_W,  PUPA_H);  break;
            default: break;
        }
    }
}

void Renderer::_draw_queen(const Chamber& ch) {
    if (!ch.queen_obj.alive) return;
    int px = ch.queen_obj.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
    int py = ch.queen_obj.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
    _draw_sprite(px, py, QUEEN, QUEEN_W, QUEEN_H);
}

void Renderer::_draw_workers(const Chamber& ch, float lerp_t) {
    float t = (lerp_t < 0.0f) ? 0.0f : ((lerp_t > 1.0f) ? 1.0f : lerp_t);
    for (int i = 0; i < ch.lil_guy_count; i++) {
        auto& w = ch.lil_guys[i];
        if (!w.alive) continue;

        float fx = w.prev_x + (w.x - w.prev_x) * t;
        float fy = w.prev_y + (w.y - w.prev_y) * t;
        // Float positions are cell-centered (cx+0.5), so no CELL_SIZE/2 offset
        int px = static_cast<int>(fx * Cfg::CELL_SIZE);
        int py = static_cast<int>(fy * Cfg::CELL_SIZE);

        if (w.role == ROLE_MAJOR)
            _draw_sprite(px, py, MAJOR, MAJOR_W, MAJOR_H);
        else if (w.is_pioneer)
            _draw_sprite(px, py, WORKER_PIONEER, WORKER_PIONEER_W, WORKER_PIONEER_H);
        else
            _draw_sprite(px, py, WORKER_MINOR, WORKER_MINOR_W, WORKER_MINOR_H);

        // Food morsel indicator
        if (w.food_carried > 0) {
            int mx = px - static_cast<int>(w.facing_dx * 4);
            int my = py - static_cast<int>(w.facing_dy * 4);
            _gfx->fillRect(mx - 1, my - 1, 3, 3, PAL_FOOD_CARRY);
            _mark_dirty(mx - 1, my - 1, 3, 3);
        }
    }
}
