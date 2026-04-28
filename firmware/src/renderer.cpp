/* Chamber renderer — 1:1 full-chamber, dirty-rect optimized.
 *
 * With CELL_SIZE=16 and grid 30x20, the full chamber fills 480x320
 * exactly. No viewport or scaling needed. Screen borders = chamber borders.
 *
 * Floor: warm textured chamber with radial gradient, grain specks,
 * four edge-decor objects, three palette states (day/dusk/night).
 *
 * CHAMBER_FLOOR_CACHED: floor is rendered to a PSRAM buffer and
 * blitted each frame. Re-rendered only when night_factor changes
 * by more than FLOOR_CACHE_THRESHOLD (0.02).
 */
#include "renderer.h"
#include "palette.h"
#include "sprites.h"
#include "time_of_day.h"
#include <pgmspace.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Compile-time flag: set to 0 to disable floor caching for A/B comparison
#ifndef CHAMBER_FLOOR_CACHED
#define CHAMBER_FLOOR_CACHED 1
#endif

// Profiling: prints frame timing breakdown to Serial every 5 seconds
#ifndef RENDERER_PROFILE
#define RENDERER_PROFILE 1
#endif

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;

// ---- Floor geometry ----
static constexpr int FLOOR_MARGIN  = 8;
static constexpr int FLOOR_RADIUS  = 6;
static constexpr int FLOOR_X       = FLOOR_MARGIN;
static constexpr int FLOOR_Y       = FLOOR_MARGIN;
static constexpr int FLOOR_W       = SCREEN_W - FLOOR_MARGIN * 2;  // 464
static constexpr int FLOOR_H       = SCREEN_H - FLOOR_MARGIN * 2;  // 304
// Gradient center aligned to queen's pixel position
static constexpr int GRAD_CX       = Cfg::QUEEN_SPAWN_X * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;  // 248
static constexpr int GRAD_CY       = Cfg::QUEEN_SPAWN_Y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;  // 168

// Floor cache invalidation threshold
static constexpr float FLOOR_CACHE_THRESHOLD = 0.02f;

// ---- Chamber palette definitions ----
static constexpr ChamberPalette CPAL_DAY = {
    0x2B, 0x24, 0x1D,
    0xE8, 0xD4, 0xA8,
    0xB8, 0x99, 0x66,
    0x6B, 0x4A, 0x2A,
    249, 228, 184, 20,
};

static constexpr ChamberPalette CPAL_DUSK = {
    0x1A, 0x16, 0x14,
    0xC9, 0xA5, 0x7A,
    0x8A, 0x6B, 0x4A,
    0x4A, 0x2F, 0x1A,
    184, 95, 62, 31,
};

static constexpr ChamberPalette CPAL_NIGHT = {
    0x0D, 0x0B, 0x10,
    0x4A, 0x48, 0x64,
    0x2D, 0x2A, 0x42,
    0x1A, 0x18, 0x28,
    80, 110, 160, 36,
};

// ---- Current interpolated chamber palette ----
static ChamberPalette _cpal;

static inline uint8_t _lerp8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)(a + (int)(((int)b - (int)a) * t));
}

static void _interpolate_chamber_palette() {
    float nf = g_tod.night_factor;

    const ChamberPalette* from;
    const ChamberPalette* to;
    float t;

    if (nf < 0.05f) {
        _cpal = CPAL_DAY;
        return;
    } else if (nf > 0.85f) {
        _cpal = CPAL_NIGHT;
        return;
    } else if (nf < 0.4f) {
        from = &CPAL_DAY; to = &CPAL_DUSK;
        t = nf / 0.4f;
    } else {
        from = &CPAL_DUSK; to = &CPAL_NIGHT;
        t = (nf - 0.4f) / 0.6f;
    }

    _cpal.outer_r  = _lerp8(from->outer_r,  to->outer_r,  t);
    _cpal.outer_g  = _lerp8(from->outer_g,  to->outer_g,  t);
    _cpal.outer_b  = _lerp8(from->outer_b,  to->outer_b,  t);
    _cpal.floor1_r = _lerp8(from->floor1_r, to->floor1_r, t);
    _cpal.floor1_g = _lerp8(from->floor1_g, to->floor1_g, t);
    _cpal.floor1_b = _lerp8(from->floor1_b, to->floor1_b, t);
    _cpal.floor2_r = _lerp8(from->floor2_r, to->floor2_r, t);
    _cpal.floor2_g = _lerp8(from->floor2_g, to->floor2_g, t);
    _cpal.floor2_b = _lerp8(from->floor2_b, to->floor2_b, t);
    _cpal.grain_r  = _lerp8(from->grain_r,  to->grain_r,  t);
    _cpal.grain_g  = _lerp8(from->grain_g,  to->grain_g,  t);
    _cpal.grain_b  = _lerp8(from->grain_b,  to->grain_b,  t);
    _cpal.amb_r    = _lerp8(from->amb_r,    to->amb_r,    t);
    _cpal.amb_g    = _lerp8(from->amb_g,    to->amb_g,    t);
    _cpal.amb_b    = _lerp8(from->amb_b,    to->amb_b,    t);
    _cpal.amb_a    = _lerp8(from->amb_a,    to->amb_a,    t);
}

static inline uint16_t _rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// ---- Night tint for sprites ----
static float    _nf;
static uint16_t tint_night(uint16_t c, float nf, float strength = 1.0f) {
    if (nf < 0.01f) return c;
    float t = nf * strength;

    int r5 = (c >> 11) & 0x1F;
    int g6 = (c >> 5)  & 0x3F;
    int b5 =  c        & 0x1F;

    float r = r5 / 31.0f;
    float g = g6 / 63.0f;
    float b = b5 / 31.0f;

    float dark = 1.0f - t * 0.75f;
    r *= dark;
    g *= dark;
    b *= dark;

    r *= (1.0f - t * 0.3f);
    b += t * 0.06f;
    if (b > 1.0f) b = 1.0f;

    int r5o = (int)(r * 31.0f + 0.5f);
    int g6o = (int)(g * 63.0f + 0.5f);
    int b5o = (int)(b * 31.0f + 0.5f);
    if (r5o > 31) r5o = 31;
    if (g6o > 63) g6o = 63;
    if (b5o > 31) b5o = 31;

    return (r5o << 11) | (g6o << 5) | b5o;
}

// Cached tinted sprite palette
static uint16_t _pal_food_dark;
static uint16_t _pal_food_light;
static uint16_t _pal_food_carry;
static uint16_t _pal_queen_head;
static uint16_t _pal_egg_colour;
static uint16_t _pal_larva_colour;
static uint16_t _pal_glow_warm;
static uint16_t _pal_glow_amber;
static uint16_t _pal_ui_alert;
static uint16_t _pal_ui_dim;

static void _update_night_palette() {
    _nf = g_tod.night_factor;
    _pal_glow_warm    = tint_night(PAL_GLOW_WARM,    _nf, 0.7f);
    _pal_glow_amber   = tint_night(PAL_GLOW_AMBER,   _nf, 0.6f);
    _pal_food_dark    = tint_night(PAL_FOOD_DARK,     _nf, 0.5f);
    _pal_food_light   = tint_night(PAL_FOOD_LIGHT,    _nf, 0.5f);
    _pal_food_carry   = tint_night(PAL_FOOD_CARRY,    _nf, 0.4f);
    _pal_queen_head   = tint_night(PAL_QUEEN_HEAD,    _nf, 0.4f);
    _pal_egg_colour   = tint_night(PAL_EGG_COLOUR,    _nf, 0.5f);
    _pal_larva_colour = tint_night(PAL_LARVA_COLOUR,  _nf, 0.5f);
    _pal_ui_alert     = tint_night(PAL_UI_ALERT,      _nf, 0.3f);
    _pal_ui_dim       = tint_night(PAL_UI_DIM,        _nf, 0.5f);
}

// ---- Sprite scale factors ----
// Biologically accurate ratios, queen = reference
// Queen at 2.0x (88px) is the 1.00 anchor; others derived from mass^(1/3)
static constexpr float SCALE_QUEEN          = 2.0f;   // 44x44 base → 88px  (1.00)
static constexpr float SCALE_WORKER_MAJOR   = 4.6f;   // 10x10 base → 46px  (0.52)
static constexpr float SCALE_WORKER_MINOR   = 3.2f;   // 10x10 base → 32px  (0.36)
static constexpr float SCALE_WORKER_PIONEER = 2.3f;   // 10x10 base → 23px  (0.26)
static constexpr float SCALE_EGG            = 3.5f;   //  4x4  base → 14px  (0.16)
static constexpr float SCALE_LARVA          = 4.4f;   //  6x6  base → 26px  (0.30)
static constexpr float SCALE_PUPA           = 3.2f;   //  8x8  base → 26px  (0.29)

// ---- Sprite frame lookup ----
// Returns sprite data for a given role + animation frame.
// Returns nullptr if no dedicated frame exists — caller falls back to BASE.
struct SpriteRef {
    const uint16_t* data;
    int w, h;
};

static const SpriteRef* _get_worker_sprite(Role /*role*/, bool /*is_pioneer*/,
                                            LilGuySpriteFrame frame) {
    static const SpriteRef base = {WORKER_PIONEER, WORKER_PIONEER_W, WORKER_PIONEER_H};
    static const SpriteRef lean = {WORKER_LEAN, WORKER_LEAN_W, WORKER_LEAN_H};
    static const SpriteRef snooze = {WORKER_SLEEP, WORKER_SLEEP_W, WORKER_SLEEP_H};
    switch (frame) {
        case LG_FRAME_BASE: return &base;
        case LG_FRAME_LEAN: return &lean;
        case LG_FRAME_SNOOZE: return &snooze;
        default: return nullptr;
    }
}

// ---- Seeded RNG for grain specks ----
static uint32_t _grain_rng(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// ================================================================
//  Floor cache (PSRAM-backed framebuffer)
// ================================================================

#if CHAMBER_FLOOR_CACHED
// Full-screen floor cache: 480x320 RGB565 = 307,200 bytes
// Allocated in PSRAM at init time.
static uint16_t* _floor_cache = nullptr;
static float     _floor_cache_nf = -1.0f;  // night_factor when cache was rendered
static bool      _floor_cache_valid = false;

static inline void _floor_cache_put(int x, int y, uint16_t col) {
    _floor_cache[y * SCREEN_W + x] = col;
}
#endif

// ================================================================
//  Profiling
// ================================================================

#if RENDERER_PROFILE
static unsigned long _prof_frame_total = 0;
static unsigned long _prof_floor_total = 0;
static unsigned long _prof_sprites_total = 0;
static unsigned long _prof_flush_total = 0;
static int           _prof_frame_count = 0;
static unsigned long _prof_last_report = 0;
#endif

// ================================================================
//  Init
// ================================================================

void Renderer::init(Arduino_Canvas* canvas) {
    _gfx = canvas;
    _needs_full_redraw = true;
    _dirty_count = 0;
    _anim_count = 0;

    // Precompute grain speck positions (deterministic)
    uint32_t seed = 0xDEADBEE5;
    for (int i = 0; i < NUM_GRAIN; i++) {
        seed = _grain_rng(seed);
        int gx = FLOOR_X + 12 + (int)(seed % (FLOOR_W - 24));
        seed = _grain_rng(seed);
        int gy = FLOOR_Y + 12 + (int)(seed % (FLOOR_H - 24));
        seed = _grain_rng(seed);
        uint8_t r10 = 6 + (seed % 13);  // 6–18 = 0.6–1.8px
        _grain[i] = { (int16_t)gx, (int16_t)gy, r10 };
    }

#if CHAMBER_FLOOR_CACHED
    // Allocate floor cache in PSRAM
    _floor_cache = (uint16_t*)ps_malloc(SCREEN_W * SCREEN_H * sizeof(uint16_t));
    if (_floor_cache) {
        Serial.printf("[renderer] Floor cache allocated: %d bytes in PSRAM\n",
                      SCREEN_W * SCREEN_H * 2);
        _floor_cache_valid = false;
        _floor_cache_nf = -1.0f;
    } else {
        Serial.println("[renderer] WARNING: PSRAM alloc failed, floor cache disabled");
    }
#endif

    // Sprout starts fresh each boot (grows via milestones during session)
    _sprout_leaf_count = Cfg::MILESTONE_LEAF_BASE;
    Serial.printf("[renderer] Sprout leaves: %d\n", _sprout_leaf_count);
}

void Renderer::flush() {
#if RENDERER_PROFILE
    unsigned long t0 = millis();
#endif
    _gfx->flush();
#if RENDERER_PROFILE
    _prof_flush_total += millis() - t0;
#endif
}

// ================================================================
//  Dirty rect management
// ================================================================

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
#if CHAMBER_FLOOR_CACHED
    if (_floor_cache && _floor_cache_valid) {
        // Restore dirty rects from floor cache — scanline blit (fast)
        uint16_t* fb = (uint16_t*)_gfx->getFramebuffer();
        for (int i = 0; i < _dirty_count; i++) {
            auto& d = _dirty[i];
            if (fb) {
                // Direct framebuffer memcpy — fastest path
                for (int row = d.y; row < d.y + d.h && row < SCREEN_H; row++) {
                    memcpy(&fb[row * SCREEN_W + d.x],
                           &_floor_cache[row * SCREEN_W + d.x],
                           d.w * sizeof(uint16_t));
                }
            } else {
                // Fallback: scanline blit via GFX API
                for (int row = d.y; row < d.y + d.h && row < SCREEN_H; row++) {
                    _gfx->draw16bitRGBBitmap(d.x, row,
                        &_floor_cache[row * SCREEN_W + d.x], d.w, 1);
                }
            }
        }
        _dirty_count = 0;
        return;
    }
#endif
    // Fallback: flat fill
    uint16_t floor_mid = _rgb565(
        (_cpal.floor1_r + _cpal.floor2_r) / 2,
        (_cpal.floor1_g + _cpal.floor2_g) / 2,
        (_cpal.floor1_b + _cpal.floor2_b) / 2);
    uint16_t outer = _rgb565(_cpal.outer_r, _cpal.outer_g, _cpal.outer_b);

    for (int i = 0; i < _dirty_count; i++) {
        auto& d = _dirty[i];
        if (d.x < FLOOR_X || d.y < FLOOR_Y ||
            d.x + d.w > FLOOR_X + FLOOR_W || d.y + d.h > FLOOR_Y + FLOOR_H) {
            _gfx->fillRect(d.x, d.y, d.w, d.h, outer);
        } else {
            _gfx->fillRect(d.x, d.y, d.w, d.h, floor_mid);
        }
    }
    _dirty_count = 0;
}

// ================================================================
//  Main draw
// ================================================================

void Renderer::draw(const Chamber& ch, float lerp_t) {
#if BOOT_SPLASH_ENABLED
    if (_boot_splash_active) {
        if (_tick_boot_splash(ch)) return;
    }
#endif

#if RENDERER_PROFILE
    unsigned long frame_start = millis();
#endif

    _update_night_palette();
    _interpolate_chamber_palette();

#if RENDERER_PROFILE
    unsigned long floor_start = millis();
#endif

#if CHAMBER_FLOOR_CACHED
    if (_floor_cache) {
        bool need_refresh = !_floor_cache_valid ||
            _needs_full_redraw ||
            fabsf(g_tod.night_factor - _floor_cache_nf) > FLOOR_CACHE_THRESHOLD;

        if (need_refresh) {
            _render_floor_to_cache();
            _floor_cache_nf = g_tod.night_factor;
            _floor_cache_valid = true;
        }

        _blit_floor_cache_full();
        _dirty_count = 0;
        _needs_full_redraw = false;
    } else
#endif
    {
        if (_needs_full_redraw) {
            uint16_t outer = _rgb565(_cpal.outer_r, _cpal.outer_g, _cpal.outer_b);
            _gfx->fillScreen(outer);
            _dirty_count = 0;
            _needs_full_redraw = false;
        } else {
            _clear_dirty();
        }
        _draw_floor_uncached();
    }

#if RENDERER_PROFILE
    unsigned long floor_end = millis();
    _prof_floor_total += floor_end - floor_start;
    unsigned long sprites_start = millis();
#endif

    // Sprout overlay (per-frame, on top of floor, below entities)
    _draw_sprout_overlay();

    // Layer 2: floor-level sprites (food, brood) — Y-sorted
    _build_floor_sprites(ch);
    _draw_sorted_sprites(_floor_sprites, _floor_sprite_count, ch);

    // Layer 3: living agents (workers, queen) — Y-sorted, queen +2 cell bias
    _build_agent_sprites(ch, lerp_t);
    _draw_sorted_sprites(_agent_sprites, _agent_sprite_count, ch);

    _draw_anims();

    // Check for milestone leaf growth
    _check_milestone(ch);

    _frame++;

#if RENDERER_PROFILE
    _prof_sprites_total += millis() - sprites_start;
    _prof_frame_total += millis() - frame_start;
    _prof_frame_count++;

    unsigned long now = millis();
    if (now - _prof_last_report >= 5000 && _prof_frame_count > 0) {
        Serial.printf("[perf] frames=%d avg_total=%lums avg_floor=%lums avg_sprites=%lums avg_flush=%lums fps=%.1f\n",
            _prof_frame_count,
            _prof_frame_total / _prof_frame_count,
            _prof_floor_total / _prof_frame_count,
            _prof_sprites_total / _prof_frame_count,
            _prof_flush_total / _prof_frame_count,
            1000.0f * _prof_frame_count / (_prof_frame_total > 0 ? _prof_frame_total : 1));
        _prof_frame_total = 0;
        _prof_floor_total = 0;
        _prof_sprites_total = 0;
        _prof_flush_total = 0;
        _prof_frame_count = 0;
        _prof_last_report = now;
    }
#endif
}

// ================================================================
//  Floor cache rendering + blitting
// ================================================================

#if CHAMBER_FLOOR_CACHED

void Renderer::_render_floor_to_cache() {
    // Render floor to the GFX canvas using its drawing API, then snapshot
    // the canvas into _floor_cache. This avoids per-pixel RGB565 banding
    // since fillCircle produces natural circular bands, not horizontal ones.

    uint16_t outer_col = _rgb565(_cpal.outer_r, _cpal.outer_g, _cpal.outer_b);
    uint16_t floor2_col = _rgb565(_cpal.floor2_r, _cpal.floor2_g, _cpal.floor2_b);

    // Pass 1: Frame
    _gfx->fillScreen(outer_col);

    // Pass 2: Base floor
    _gfx->fillRoundRect(FLOOR_X, FLOOR_Y, FLOOR_W, FLOOR_H, FLOOR_RADIUS, floor2_col);

    // Pass 3: Radial gradient via concentric filled circles (painter's algorithm).
    // 32 bands from outer (floor2) to inner (floor1). Circular band boundaries
    // look natural on RGB565 where horizontal bands look terrible.
    static constexpr int NUM_BANDS = 32;
    static constexpr int MAX_RAD = 280;
    for (int band = 0; band < NUM_BANDS; band++) {
        float t = (float)band / (NUM_BANDS - 1);  // 0 = outermost, 1 = center
        int radius = MAX_RAD - (int)(MAX_RAD * t * 0.92f);
        uint8_t r = _lerp8(_cpal.floor2_r, _cpal.floor1_r, t);
        uint8_t g = _lerp8(_cpal.floor2_g, _cpal.floor1_g, t);
        uint8_t b = _lerp8(_cpal.floor2_b, _cpal.floor1_b, t);
        _gfx->fillCircle(GRAD_CX, GRAD_CY, radius, _rgb565(r, g, b));
    }

    // Pass 4: Grain specks (28% opacity, pre-blended against mid-floor)
    uint8_t gr = (uint8_t)(_cpal.grain_r * 0.28f + ((_cpal.floor1_r + _cpal.floor2_r) / 2) * 0.72f);
    uint8_t gg = (uint8_t)(_cpal.grain_g * 0.28f + ((_cpal.floor1_g + _cpal.floor2_g) / 2) * 0.72f);
    uint8_t gb = (uint8_t)(_cpal.grain_b * 0.28f + ((_cpal.floor1_b + _cpal.floor2_b) / 2) * 0.72f);
    uint16_t grain_col = _rgb565(gr, gg, gb);
    for (int i = 0; i < NUM_GRAIN; i++) {
        auto& s = _grain[i];
        if (s.radius_x10 <= 10) {
            _gfx->drawPixel(s.x, s.y, grain_col);
        } else {
            int r = s.radius_x10 / 10;
            if (r < 1) r = 1;
            _gfx->fillCircle(s.x, s.y, r, grain_col);
        }
    }

    // Pass 5: Edge decor (draw directly to canvas)
    _draw_edge_decor_direct();

    // Snapshot canvas → cache buffer
    uint16_t* fb = (uint16_t*)_gfx->getFramebuffer();
    if (fb) {
        memcpy(_floor_cache, fb, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    }
}

void Renderer::_blit_floor_cache_full() {
    uint16_t* fb = (uint16_t*)_gfx->getFramebuffer();
    if (fb) {
        memcpy(fb, _floor_cache, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    } else {
        for (int y = 0; y < SCREEN_H; y++) {
            _gfx->draw16bitRGBBitmap(0, y, &_floor_cache[y * SCREEN_W], SCREEN_W, 1);
        }
    }
}


#endif // CHAMBER_FLOOR_CACHED

// ================================================================
//  Uncached floor rendering (fallback / debug path)
// ================================================================

void Renderer::_draw_floor_uncached() {
    // Pass 2: Base
    uint16_t floor2_col = _rgb565(_cpal.floor2_r, _cpal.floor2_g, _cpal.floor2_b);
    _gfx->fillRoundRect(FLOOR_X, FLOOR_Y, FLOOR_W, FLOOR_H, FLOOR_RADIUS, floor2_col);

    // Pass 3: Gradient (banded approximation for speed)
    static constexpr int NUM_BANDS = 12;
    static constexpr int MAX_RAD = 280;
    for (int band = 0; band < NUM_BANDS; band++) {
        float t = (float)band / (NUM_BANDS - 1);
        int radius = MAX_RAD - (int)(MAX_RAD * t * 0.92f);
        uint8_t r = _lerp8(_cpal.floor2_r, _cpal.floor1_r, t);
        uint8_t g = _lerp8(_cpal.floor2_g, _cpal.floor1_g, t);
        uint8_t b = _lerp8(_cpal.floor2_b, _cpal.floor1_b, t);
        _gfx->fillCircle(GRAD_CX, GRAD_CY, radius, _rgb565(r, g, b));
    }

    // Pass 4: Grain specks
    uint8_t gr = (uint8_t)(_cpal.grain_r * 0.28f + ((_cpal.floor1_r + _cpal.floor2_r) / 2) * 0.72f);
    uint8_t gg = (uint8_t)(_cpal.grain_g * 0.28f + ((_cpal.floor1_g + _cpal.floor2_g) / 2) * 0.72f);
    uint8_t gb = (uint8_t)(_cpal.grain_b * 0.28f + ((_cpal.floor1_b + _cpal.floor2_b) / 2) * 0.72f);
    uint16_t grain_col = _rgb565(gr, gg, gb);

    for (int i = 0; i < NUM_GRAIN; i++) {
        auto& s = _grain[i];
        if (s.radius_x10 <= 10) {
            _gfx->drawPixel(s.x, s.y, grain_col);
        } else {
            int r = s.radius_x10 / 10;
            if (r < 1) r = 1;
            _gfx->fillCircle(s.x, s.y, r, grain_col);
        }
    }

    // Pass 5: Edge decor (draw directly to canvas)
    _draw_edge_decor_direct();
}

void Renderer::_draw_edge_decor_direct() {
    // Shared pebble palette
    uint8_t pr = _lerp8(160, 90, _nf);
    uint8_t pg = _lerp8(155, 88, _nf);
    uint8_t pb = _lerp8(148, 95, _nf);
    uint16_t pebble_col  = _rgb565(pr, pg, pb);
    uint16_t pebble_dark = _rgb565(pr * 3/4, pg * 3/4, pb * 3/4);
    uint16_t pebble_lite = _rgb565(
        (uint8_t)fminf(255, pr * 1.15f),
        (uint8_t)fminf(255, pg * 1.10f),
        (uint8_t)fminf(255, pb * 1.05f));

    // Main pebble — top-right, cool grey rounded lump
    {
        int px = SCREEN_W - 54, py = 28;
        _gfx->fillRoundRect(px - 5, py - 3, 10, 7, 3, pebble_col);
        _gfx->drawFastHLine(px - 4, py + 3, 8, pebble_dark);
        _gfx->drawFastHLine(px - 3, py + 4, 6, pebble_dark);
    }

    // Extra pebble — bottom-right, smaller
    {
        int px = SCREEN_W - 40, py = SCREEN_H - 48;
        _gfx->fillRoundRect(px - 3, py - 2, 7, 5, 2, pebble_col);
        _gfx->drawFastHLine(px - 2, py + 2, 5, pebble_dark);
    }

    // Extra pebble — top-left area, small round
    {
        int px = 48, py = 42;
        _gfx->fillCircle(px, py, 3, pebble_col);
        _gfx->drawPixel(px - 1, py - 2, pebble_lite);
        _gfx->drawPixel(px + 1, py + 2, pebble_dark);
    }

    // Extra pebble — left side, tiny
    {
        int px = 22, py = SCREEN_H / 2 + 30;
        _gfx->fillRoundRect(px - 2, py - 1, 5, 3, 1, pebble_dark);
        _gfx->drawPixel(px, py - 1, pebble_lite);
    }

    // Seed — bottom-left, brown ellipse with crack
    {
        int sx = 34, sy = SCREEN_H - 34;
        uint8_t sr = _lerp8(140, 60, _nf);
        uint8_t sg = _lerp8(90, 45, _nf);
        uint8_t sb = _lerp8(50, 35, _nf);
        uint16_t seed_col = _rgb565(sr, sg, sb);
        uint16_t seed_dark = _rgb565(sr * 2/3, sg * 2/3, sb * 2/3);
        uint16_t seed_light = _rgb565(
            (uint8_t)fminf(255, sr * 1.2f),
            (uint8_t)fminf(255, sg * 1.1f),
            sb);
        _gfx->fillRoundRect(sx - 4, sy - 2, 8, 5, 2, seed_col);
        _gfx->drawPixel(sx - 1, sy - 2, seed_light);
        _gfx->drawPixel(sx,     sy - 2, seed_light);
        _gfx->drawPixel(sx + 1, sy - 1, seed_dark);
        _gfx->drawPixel(sx + 2, sy,     seed_dark);
        _gfx->drawPixel(sx + 2, sy + 1, seed_dark);
    }

    // Moss — four small dots
    {
        int mx = SCREEN_W - 88, my = SCREEN_H - 38;
        uint8_t mr = (uint8_t)(_lerp8(220, 140, _nf) * 0.70f + _cpal.floor2_r * 0.30f);
        uint8_t mg = (uint8_t)(_lerp8(210, 160, _nf) * 0.70f + _cpal.floor2_g * 0.30f);
        uint8_t mb = (uint8_t)(_lerp8(190, 140, _nf) * 0.70f + _cpal.floor2_b * 0.30f);
        uint16_t moss_col = _rgb565(mr, mg, mb);
        _gfx->drawPixel(mx,     my,     moss_col);
        _gfx->drawPixel(mx + 3, my - 1, moss_col);
        _gfx->drawPixel(mx + 1, my + 2, moss_col);
        _gfx->drawPixel(mx + 4, my + 1, moss_col);
    }

    // Sprout is drawn per-frame as an overlay (not cached) — see _draw_sprout_overlay()
}

// ================================================================
//  Sprout overlay (per-frame, sun-tracking, milestone leaves)
// ================================================================

static constexpr int SPROUT_X = SCREEN_W - 24;
static constexpr int SPROUT_Y = SCREEN_H - 50;

void Renderer::_draw_sprout_overlay() {
    // Sun-tracking tilt: sinusoidal curve across day_progress
    // day_progress 0.0=sunrise, 0.5=noon, 1.0=sunset
    // Tilt west at sunrise (negative), upright at noon, east at sunset (positive)
    // At night: droop slightly
    float tilt;
    if (g_tod.night_factor > 0.5f) {
        // Night: gentle droop
        tilt = -0.15f;
    } else {
        // Day: track sun with smooth sinusoidal curve
        float sun = (g_tod.day_progress - 0.5f) * 2.0f;  // -1 to +1
        // Smoothstep-ish: use sine for ease in/out
        tilt = sinf(sun * 1.5708f) * 0.35f;  // ±0.35 rad (~20°)
        // Blend toward droop as night approaches
        tilt = tilt * (1.0f - g_tod.night_factor * 2.0f);
    }

    // Advance shimmer/grow animations
    if (_leaf_grow_t >= 0.0f) {
        _leaf_grow_t += 0.033f;  // ~0.5s at 30fps
        if (_leaf_grow_t >= 1.0f) _leaf_grow_t = -1.0f;
    }
    if (_leaf_shimmer_t >= 0.0f) {
        _leaf_shimmer_t += 0.016f;  // ~2s at 30fps
        if (_leaf_shimmer_t >= 1.0f) _leaf_shimmer_t = -1.0f;
    }

    _draw_sprout_direct(SPROUT_X, SPROUT_Y, tilt, _sprout_leaf_count,
                        _leaf_grow_t, _leaf_shimmer_t);
}

void Renderer::_draw_sprout_direct(int x, int y, float tilt_angle, int leaf_count,
                                   float grow_t, float shimmer_t) {
    uint8_t stem_r = _lerp8(60, 40, _nf);
    uint8_t stem_g = _lerp8(130, 70, _nf);
    uint8_t stem_b = _lerp8(50, 45, _nf);
    uint16_t stem_col = _rgb565(stem_r, stem_g, stem_b);

    uint8_t leaf_r = _lerp8(80, 50, _nf);
    uint8_t leaf_g = _lerp8(160, 90, _nf);
    uint8_t leaf_b = _lerp8(60, 55, _nf);
    uint16_t leaf_col = _rgb565(leaf_r, leaf_g, leaf_b);
    uint16_t leaf_dark = _rgb565(leaf_r * 3/4, leaf_g * 3/4, leaf_b * 3/4);
    uint16_t leaf_lite = _rgb565(
        (uint8_t)fminf(255, leaf_r * 1.2f),
        (uint8_t)fminf(255, leaf_g * 1.15f),
        leaf_b);

    // Gold shimmer color
    uint16_t gold = _rgb565(
        _lerp8(leaf_r, 255, 0.6f),
        _lerp8(leaf_g, 200, 0.5f),
        _lerp8(leaf_b, 50, 0.3f));

    int tilt_px = (int)(sinf(tilt_angle) * 5.0f);
    int stem_h = 24;

    // Stem
    for (int dy = 0; dy < stem_h; dy++) {
        int sx = x + (int)(tilt_px * (float)dy / stem_h);
        _gfx->drawPixel(sx, y - dy, stem_col);
        if (dy < 10) _gfx->drawPixel(sx + 1, y - dy, stem_col);
        if (dy < 4)  _gfx->drawPixel(sx + 2, y - dy, stem_col);
    }

    // Leaves — dynamic spacing for higher counts
    int spacing = (leaf_count <= 4) ? 7 : (leaf_count <= 6 ? 5 : 4);
    int draw_count = (leaf_count > 7) ? 7 : leaf_count;

    for (int lf = 0; lf < draw_count; lf++) {
        int ly = y - 8 - lf * spacing;
        int lx = x + (int)(tilt_px * (float)(y - ly) / stem_h);
        bool right = (lf % 2 == 0);
        int dir = right ? 1 : -1;

        // If this is the newest leaf and it's growing, scale it
        bool is_growing = (lf == draw_count - 1 && grow_t >= 0.0f);
        float scale = is_growing ? (grow_t < 1.0f ? grow_t * grow_t * (3.0f - 2.0f * grow_t) : 1.0f) : 1.0f;

        // Determine pixel color: gold shimmer sweep from base to tip
        bool shimmer_active = (shimmer_t >= 0.0f && shimmer_t < 1.0f);
        float shimmer_strength = 0.0f;
        if (shimmer_active) {
            // Shimmer sweeps from base (lf=0) to tip (lf=max) over the animation
            float leaf_pos = (float)lf / fmaxf(1.0f, (float)(draw_count - 1));
            float wave = sinf((shimmer_t * 2.0f - leaf_pos) * 3.14159f);
            shimmer_strength = fmaxf(0.0f, wave) * (1.0f - shimmer_t * 0.5f);
        }

        uint16_t lc  = shimmer_strength > 0.01f ?
            _rgb565(_lerp8(leaf_r, 255, shimmer_strength * 0.6f),
                    _lerp8(leaf_g, 200, shimmer_strength * 0.5f),
                    _lerp8(leaf_b, 50,  shimmer_strength * 0.3f)) : leaf_col;
        uint16_t ld  = shimmer_strength > 0.01f ? lc : leaf_dark;
        uint16_t ll  = shimmer_strength > 0.01f ? gold : leaf_lite;

        if (scale < 0.2f) continue;  // too small to draw

        // Leaf body
        _gfx->drawPixel(lx + dir * 1, ly - 1, lc);
        _gfx->drawPixel(lx + dir * 2, ly - 1, lc);
        if (scale > 0.5f) _gfx->drawPixel(lx + dir * 3, ly - 1, ll);
        _gfx->drawPixel(lx + dir * 1, ly,     lc);
        _gfx->drawPixel(lx + dir * 2, ly,     ld);  // vein
        _gfx->drawPixel(lx + dir * 3, ly,     lc);
        if (scale > 0.5f) {
            _gfx->drawPixel(lx + dir * 4, ly, lc);
            _gfx->drawPixel(lx + dir * 5, ly, ld);
        }
        _gfx->drawPixel(lx + dir * 1, ly + 1, lc);
        _gfx->drawPixel(lx + dir * 2, ly + 1, lc);
        if (scale > 0.5f) {
            _gfx->drawPixel(lx + dir * 3, ly + 1, ld);
            _gfx->drawPixel(lx + dir * 4, ly + 1, ld);
        }
    }

    // Tip bud
    int tip_x = x + tilt_px;
    int tip_y = y - stem_h;
    _gfx->drawPixel(tip_x,     tip_y,     leaf_col);
    _gfx->drawPixel(tip_x,     tip_y - 1, leaf_col);
    _gfx->drawPixel(tip_x + 1, tip_y,     leaf_lite);
    _gfx->drawPixel(tip_x - 1, tip_y,     leaf_lite);
    _gfx->drawPixel(tip_x,     tip_y - 2, leaf_lite);
}

// ================================================================
//  Milestone leaf check
// ================================================================

void Renderer::_check_milestone(const Chamber& ch) {
    uint16_t born = ch.colony->total_workers_born;
    if (born <= _last_milestone_born) return;
    _last_milestone_born = born;

    int target = Cfg::MILESTONE_LEAF_BASE + born / Cfg::MILESTONE_LEAF_INTERVAL;
    if (target > Cfg::MILESTONE_LEAF_CAP) target = Cfg::MILESTONE_LEAF_CAP;

    if (target > _sprout_leaf_count) {
        _sprout_leaf_count = target;
        _leaf_grow_t = 0.0f;
        _leaf_shimmer_t = 0.0f;

        Serial.printf("[milestone] New leaf! leaves=%d (workers born=%d)\n",
                      _sprout_leaf_count, born);
    }
}

void Renderer::reset_sprout() {
    _sprout_leaf_count = Cfg::MILESTONE_LEAF_BASE;
    _last_milestone_born = 0;
    _leaf_grow_t = -1.0f;
    _leaf_shimmer_t = -1.0f;
    Serial.println("[renderer] Sprout reset to base");
}

// ================================================================
//  Boot splash
// ================================================================

void Renderer::start_boot_splash() {
#if BOOT_SPLASH_ENABLED
    _boot_splash_active = true;
    _boot_splash_start_ms = millis();
    _gfx->fillScreen(0x0000);
    _gfx->flush();
#endif
}

bool Renderer::_tick_boot_splash(const Chamber& ch) {
    unsigned long elapsed = millis() - _boot_splash_start_ms;

    _update_night_palette();
    _interpolate_chamber_palette();

    if (elapsed < 300) {
        // Phase 0: hold black
        _gfx->fillScreen(0x0000);
    } else if (elapsed < 1800) {
        // Phase 1: render full scene, fade in from black over 1500ms
        float fade = (float)(elapsed - 300) / 1500.0f;
        if (fade > 1.0f) fade = 1.0f;

#if CHAMBER_FLOOR_CACHED
        if (_floor_cache && !_floor_cache_valid) {
            _render_floor_to_cache();
            _floor_cache_nf = g_tod.night_factor;
            _floor_cache_valid = true;
        }
        if (_floor_cache) _blit_floor_cache_full();
#else
        uint16_t outer = _rgb565(_cpal.outer_r, _cpal.outer_g, _cpal.outer_b);
        _gfx->fillScreen(outer);
        _draw_floor_uncached();
#endif
        _draw_sprout_overlay();
        if (ch.has_queen) _draw_queen(ch);

        // Dim entire framebuffer to create smooth fade-in
        _dim_framebuffer(fade);
    } else {
        // Phase 2: splash done, hand off to normal rendering
        _boot_splash_active = false;
        _needs_full_redraw = false;
        _dirty_count = 0;
        return false;
    }

    _frame++;
    return true;  // splash handled this frame
}

void Renderer::_dim_framebuffer(float brightness) {
    uint16_t* buf = _gfx->getFramebuffer();
    if (!buf) return;
    const int total = SCREEN_W * SCREEN_H;
    // Fixed-point brightness: 0–256 maps to 0.0–1.0
    const int bfp = (int)(brightness * 256.0f);
    for (int i = 0; i < total; i++) {
        uint16_t px = buf[i];
        uint8_t r = (px >> 11) & 0x1F;
        uint8_t g = (px >> 5)  & 0x3F;
        uint8_t b =  px        & 0x1F;
        r = (r * bfp) >> 8;
        g = (g * bfp) >> 8;
        b = (b * bfp) >> 8;
        buf[i] = (r << 11) | (g << 5) | b;
    }
}

// ================================================================
//  Z-sorted sprite rendering
// ================================================================

static bool _sprite_cmp(const SpriteDraw& a, const SpriteDraw& b) {
    if (a.sort_y != b.sort_y) return a.sort_y < b.sort_y;  // smaller Y = further back
    if (a.size_px != b.size_px) return a.size_px < b.size_px;  // larger sprite drawn on top
    return a.entity_idx < b.entity_idx;  // stable tiebreaker
}

void Renderer::_build_floor_sprites(const Chamber& ch) {
    _floor_sprite_count = 0;

    // Food piles
    for (int i = 0; i < ch.food_pile_count; i++) {
        if (ch.food_piles[i].amount <= 0) continue;
        auto& sd = _floor_sprites[_floor_sprite_count++];
        sd.sort_y   = ch.food_piles[i].y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.render_x = ch.food_piles[i].x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.render_y = static_cast<int16_t>(sd.sort_y);
        sd.size_px  = 8;
        sd.kind     = SK_FOOD_PILE;
        sd.flags    = 0;
        sd.entity_idx = i;
    }

    // Brood
    for (int i = 0; i < ch.brood_count; i++) {
        auto& b = ch.brood[i];
        if (!b.alive()) continue;
        auto& sd = _floor_sprites[_floor_sprite_count++];
        sd.render_x = b.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.render_y = b.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.sort_y   = sd.render_y;
        sd.flags    = 0;
        sd.entity_idx = i;
        switch (b.stage) {
            case STAGE_EGG:
                sd.kind = SK_EGG;
                sd.size_px = static_cast<uint16_t>(EGG_W * SCALE_EGG + 0.5f);
                break;
            case STAGE_LARVA:
                sd.kind = SK_LARVA;
                sd.size_px = static_cast<uint16_t>(LARVA_W * SCALE_LARVA + 0.5f);
                break;
            case STAGE_PUPA:
                sd.kind = SK_PUPA;
                sd.size_px = static_cast<uint16_t>(PUPA_W * SCALE_PUPA + 0.5f);
                break;
            default: _floor_sprite_count--; break;
        }
    }

    std::stable_sort(_floor_sprites, _floor_sprites + _floor_sprite_count, _sprite_cmp);
}

void Renderer::_build_agent_sprites(const Chamber& ch, float lerp_t) {
    _agent_sprite_count = 0;
    float t = (lerp_t < 0.0f) ? 0.0f : ((lerp_t > 1.0f) ? 1.0f : lerp_t);

    // Workers
    for (int i = 0; i < ch.lil_guy_count; i++) {
        auto& w = ch.lil_guys[i];
        if (!w.alive) continue;

        float fx = w.prev_x + (w.x - w.prev_x) * t;
        float fy = w.prev_y + (w.y - w.prev_y) * t;
        int px = static_cast<int>(fx * Cfg::CELL_SIZE);
        int py = static_cast<int>(fy * Cfg::CELL_SIZE);
        float sort_y_stable = fy * Cfg::CELL_SIZE;  // depth key before bob/anim offsets

        // Animation override: interaction animations suppress normal bob
        if (w.stack_on >= 0) {
            // Stacked ant: walk down the tower, summing each ant's sprite height
            float offset = 0.0f;
            int stack_depth = 0;
            int base_idx = w.stack_on;
            int cur = w.stack_on;
            while (cur >= 0 && offset < 200.0f) {
                auto& b = ch.lil_guys[cur];
                float s = SCALE_WORKER_MINOR;
                if (b.role == ROLE_MAJOR)  s = SCALE_WORKER_MAJOR;
                else if (b.is_pioneer)     s = SCALE_WORKER_PIONEER;
                offset -= static_cast<int>(WORKER_PIONEER_H * s + 0.5f) * 0.6f;
                stack_depth++;
                if (b.stack_on < 0) base_idx = cur;  // ground-level ant
                cur = b.stack_on;
            }
            // Sort by base ant's position so whole tower moves as one z-unit
            auto& base = ch.lil_guys[base_idx];
            float base_fy = base.prev_y + (base.y - base.prev_y) * t;
            sort_y_stable = base_fy * Cfg::CELL_SIZE + stack_depth * 0.1f;

            // Topple wobble: increasing amplitude, higher ants wobble more
            if (w.anim_type == LG_ANIM_TOPPLE) {
                float p = 1.0f - static_cast<float>(w.anim_remaining_ticks)
                               / static_cast<float>(Cfg::STACK_TOPPLE_TICKS);
                float amplitude = p * (2.0f + stack_depth * 1.5f);
                float wobble = sinf(p * 18.0f + stack_depth * 1.2f) * amplitude;
                px += static_cast<int>(wobble);
            // Damped hop during mount animation
            } else if (w.stack_hop_remaining > 0) {
                float p = 1.0f - static_cast<float>(w.stack_hop_remaining) / 12.0f;
                float bounce = expf(-3.5f * p) * cosf(p * 10.0f);
                offset += bounce * 6.0f;
            }
            py += static_cast<int>(offset);

        } else if (w.anim_type == LG_ANIM_TOPPLE) {
            if (w.anim_remaining_ticks > Cfg::STACK_FALL_TICKS) {
                // Wobble phase (ground ant)
                float total = static_cast<float>(Cfg::STACK_TOPPLE_TICKS);
                float remaining_wobble = w.anim_remaining_ticks - Cfg::STACK_FALL_TICKS;
                float p = 1.0f - remaining_wobble / total;
                float wobble = sinf(p * 18.0f) * p * 2.0f;
                px += static_cast<int>(wobble);
            } else if (w.topple_depth > 0) {
                // Fall phase: animate from stack height down to ground
                float fall_p = 1.0f - static_cast<float>(w.anim_remaining_ticks)
                                    / static_cast<float>(Cfg::STACK_FALL_TICKS);
                // Compute the height this ant was at
                float height = 0.0f;
                for (int d = 0; d < w.topple_depth; d++) {
                    float s = SCALE_WORKER_MINOR;
                    height -= static_cast<int>(WORKER_PIONEER_H * s + 0.5f) * 0.6f;
                }
                py += static_cast<int>(height * (1.0f - fall_p));
                // Alternate left/right by depth, scatter ~1 cell width
                float side = (w.topple_depth & 1) ? -1.0f : 1.0f;
                float scatter = side * Cfg::CELL_SIZE * fall_p;
                px += static_cast<int>(scatter);
            }

            // Grooming while stacked: just swap to lean sprite, no position change
        } else if (w.anim_type == LG_ANIM_GROOMING) {
            // Lean toward brood/queen: in (0-33%), hold (33-66%), out (66-100%)
            float p = 1.0f - static_cast<float>(w.anim_remaining_ticks)
                           / static_cast<float>(Cfg::GREETING_DURATION_TICKS);
            float lean;
            if (p < 0.33f) {
                float t2 = p / 0.33f;
                lean = t2 * t2 * (3.0f - 2.0f * t2);
            } else if (p < 0.66f) {
                lean = 1.0f;
            } else {
                float t2 = (p - 0.66f) / 0.34f;
                lean = 1.0f - t2 * t2 * (3.0f - 2.0f * t2);
            }
            px += static_cast<int>(w.anim_lean_dx * 2.0f * lean);
            py += static_cast<int>(w.anim_lean_dy * 2.0f * lean);
        } else if (w.anim_type == LG_ANIM_FOOD_SHARE_GIVER
                || w.anim_type == LG_ANIM_FOOD_SHARE_RECEIVER
                || w.anim_type == LG_ANIM_SNOOZE) {
            // Stationary, no bob
        } else {
            bool moving = w.has_target_cell ||
                          (w.x != w.prev_x || w.y != w.prev_y);
            bool resting = (w.state == STATE_IDLE && w.idle_ticks_remaining > 0);

            if (resting) {
                int breath_phase = (_frame + i * 7) % 60;
                int bob = (breath_phase < 30) ? 0 : -1;
                py += bob;
            } else if (moving) {
                float phase = (fx + fy) * 2.0f;
                int bob = (static_cast<int>(phase) & 1) ? -1 : 0;
                py += bob;
            }
        }

        float scale = SCALE_WORKER_MINOR;
        if (w.role == ROLE_MAJOR)  scale = SCALE_WORKER_MAJOR;
        else if (w.is_pioneer)     scale = SCALE_WORKER_PIONEER;

        auto& sd = _agent_sprites[_agent_sprite_count++];
        sd.sort_y    = sort_y_stable;
        sd.render_x  = px;
        sd.render_y  = py;
        sd.size_px   = static_cast<uint16_t>(WORKER_PIONEER_W * scale + 0.5f);
        sd.kind      = SK_WORKER;
        sd.flags     = (w.facing_dx < -0.1f) ? 1 : 0;
        sd.entity_idx = i;
    }

    // Queen
    if (ch.has_queen && ch.queen_obj.alive) {
        int px = ch.queen_obj.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int py = ch.queen_obj.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        float queen_sort_y = static_cast<float>(py) + 2.0f * Cfg::CELL_SIZE; // +2 cell bias (stable)
        int breath_phase = _frame % 60;
        int bob = (breath_phase < 30) ? 0 : -1;
        py += bob;

        auto& sd = _agent_sprites[_agent_sprite_count++];
        sd.sort_y    = queen_sort_y;
        sd.render_x  = px;
        sd.render_y  = py;
        sd.size_px   = static_cast<uint16_t>(QUEEN_W * SCALE_QUEEN + 0.5f);
        sd.kind      = SK_QUEEN;
        sd.flags     = 0;
        sd.entity_idx = -1;
    }

    std::stable_sort(_agent_sprites, _agent_sprites + _agent_sprite_count, _sprite_cmp);
}

void Renderer::_draw_sorted_sprites(SpriteDraw* list, int count, const Chamber& ch) {
    for (int i = 0; i < count; i++) {
        _draw_one_sprite(list[i], ch);
    }
}

void Renderer::_draw_one_sprite(const SpriteDraw& sd, const Chamber& ch) {
    bool flip = (sd.flags & 1);
    switch (sd.kind) {
        case SK_FOOD_PILE: {
            int cx = sd.render_x, cy = sd.render_y;
            float amt = ch.food_piles[sd.entity_idx].amount;
            _mark_dirty(cx - 6, cy - 4, 12, 8);
            if (amt <= 15) {
                _gfx->fillRect(cx - 2, cy - 1, 4, 3, _pal_food_light);
            } else if (amt <= 50) {
                _gfx->fillRect(cx - 4, cy - 2, 4, 3, _pal_food_dark);
                _gfx->fillRect(cx,     cy,     4, 3, _pal_food_light);
            } else {
                _gfx->fillRect(cx - 4, cy - 2, 4, 3, _pal_food_dark);
                _gfx->fillRect(cx,     cy,     4, 3, _pal_food_light);
                _gfx->fillRect(cx + 2, cy - 2, 4, 3, _pal_food_dark);
                _gfx->fillRect(cx - 2, cy + 2, 4, 3, _pal_food_light);
            }
            break;
        }
        case SK_EGG:
            _draw_sprite_scaled(sd.render_x, sd.render_y, EGG, EGG_W, EGG_H, SCALE_EGG);
            break;
        case SK_LARVA:
            _draw_sprite_scaled(sd.render_x, sd.render_y, LARVA, LARVA_W, LARVA_H, SCALE_LARVA);
            break;
        case SK_PUPA:
            _draw_sprite_scaled(sd.render_x, sd.render_y, PUPA, PUPA_W, PUPA_H, SCALE_PUPA);
            break;
        case SK_QUEEN:
            _draw_sprite_scaled(sd.render_x, sd.render_y, QUEEN, QUEEN_W, QUEEN_H, SCALE_QUEEN);
            break;
        case SK_WORKER: {
            auto& w = ch.lil_guys[sd.entity_idx];
            float scale = SCALE_WORKER_MINOR;
            if (w.role == ROLE_MAJOR)  scale = SCALE_WORKER_MAJOR;
            else if (w.is_pioneer)     scale = SCALE_WORKER_PIONEER;

            // Sprite frame lookup: use dedicated frame if available, else base
            LilGuySpriteFrame frame = LG_FRAME_BASE;
            if (w.anim_type == LG_ANIM_GROOMING) frame = LG_FRAME_LEAN;
            else if (w.anim_type == LG_ANIM_SNOOZE) frame = LG_FRAME_SNOOZE;
            const SpriteRef* spr = _get_worker_sprite(w.role, w.is_pioneer, frame);
            if (!spr) spr = _get_worker_sprite(w.role, w.is_pioneer, LG_FRAME_BASE);

            _draw_sprite_scaled(sd.render_x, sd.render_y, spr->data,
                                spr->w, spr->h, scale, flip);

            // Food-share receiver pulse: warm dot during first half of animation
            if (w.anim_type == LG_ANIM_FOOD_SHARE_RECEIVER
                && w.anim_remaining_ticks > Cfg::FOOD_SHARE_DURATION_TICKS / 2) {
                _gfx->fillRect(sd.render_x - 1, sd.render_y - 1, 3, 3, _pal_food_carry);
                _mark_dirty(sd.render_x - 1, sd.render_y - 1, 3, 3);
            }

            if (w.food_carried > 0) {
                int mx = sd.render_x - static_cast<int>(w.facing_dx * 4);
                int my = sd.render_y - static_cast<int>(w.facing_dy * 4);
                _gfx->fillRect(mx - 1, my - 1, 3, 3, _pal_food_carry);
                _mark_dirty(mx - 1, my - 1, 3, 3);
            }

            // Floating Zs above sleeping ants
            if (w.anim_type == LG_ANIM_SNOOZE) {
                unsigned long ms = millis();
                uint16_t zcol = _rgb565(180, 180, 220);
                _gfx->setTextSize(1);
                // Two Zs at different phases, floating upward
                for (int zi = 0; zi < 2; zi++) {
                    float phase = ((ms + zi * 1500) % 3000) / 3000.0f;
                    int zx = sd.render_x + 4 + zi * 5;
                    int zy = sd.render_y - 10 - static_cast<int>(phase * 14.0f);
                    uint8_t alpha = (phase < 0.7f) ? 255 : static_cast<uint8_t>(255 * (1.0f - (phase - 0.7f) / 0.3f));
                    uint16_t c = _rgb565(alpha * 180 / 255, alpha * 180 / 255, alpha * 220 / 255);
                    _gfx->setTextColor(c);
                    _gfx->setCursor(zx, zy);
                    _gfx->print(zi == 0 ? "z" : "Z");
                    _mark_dirty(zx, zy, 6, 8);
                }
            }
            break;
        }
    }
}

// ================================================================
//  Sprites — scanline-buffered with optional nearest-neighbor scaling
// ================================================================

static constexpr int MAX_SCALED_DIM = 96;

void Renderer::_draw_sprite_scaled(int cx, int cy, const uint16_t* data,
                                   int sw, int sh, float scale,
                                   bool flip_h) {
    int dw = static_cast<int>(sw * scale + 0.5f);
    int dh = static_cast<int>(sh * scale + 0.5f);
    if (dw > MAX_SCALED_DIM) dw = MAX_SCALED_DIM;
    if (dh > MAX_SCALED_DIM) dh = MAX_SCALED_DIM;

    int ox = cx - dw / 2;
    int oy = cy - dh / 2;
    _mark_dirty(ox, oy, dw, dh);

    bool tint = (_nf > 0.01f);
    uint16_t row_buf[MAX_SCALED_DIM];

    for (int dy = 0; dy < dh; dy++) {
        int py = oy + dy;
        if (py < 0 || py >= SCREEN_H) continue;
        int sy = dy * sh / dh;

        // Clamp horizontal span to screen
        int dx_start = (ox < 0) ? -ox : 0;
        int dx_end   = (ox + dw > SCREEN_W) ? (SCREEN_W - ox) : dw;

        // Build scanline and blit contiguous opaque runs
        // Uses draw16bitRGBBitmap which respects display rotation
        int run_start = -1;
        for (int dx = dx_start; dx < dx_end; dx++) {
            int sx = flip_h ? (sw - 1 - dx * sw / dw) : (dx * sw / dw);
            uint16_t c = pgm_read_word(&data[sy * sw + sx]);
            if (c == SPRITE_TRANSPARENT) {
                if (run_start >= 0) {
                    _gfx->draw16bitRGBBitmap(ox + run_start, py,
                        &row_buf[run_start], dx - run_start, 1);
                    run_start = -1;
                }
                continue;
            }
            if (tint) c = tint_night(c, _nf, 0.4f);
            row_buf[dx] = c;
            if (run_start < 0) run_start = dx;
        }
        if (run_start >= 0) {
            _gfx->draw16bitRGBBitmap(ox + run_start, py,
                &row_buf[run_start], dx_end - run_start, 1);
        }
    }
}

void Renderer::_draw_sprite(int cx, int cy, const uint16_t* data,
                            int sw, int sh, bool flip_h) {
    _draw_sprite_scaled(cx, cy, data, sw, sh, 1.0f, flip_h);
}

// Boot splash queen draw (simple, unsorted)
void Renderer::_draw_queen(const Chamber& ch) {
    if (!ch.queen_obj.alive) return;
    int px = ch.queen_obj.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
    int py = ch.queen_obj.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
    _draw_sprite_scaled(px, py, QUEEN, QUEEN_W, QUEEN_H, SCALE_QUEEN);
}

// ================================================================
//  Animation system
// ================================================================

void Renderer::_spawn_anim(AnimType type, int px, int py, uint8_t duration) {
    if (_anim_count < MAX_ANIMS) {
        _anims[_anim_count++] = {type, static_cast<int16_t>(px),
                                 static_cast<int16_t>(py), 0, duration, true};
        return;
    }
    int oldest = 0;
    uint8_t most_aged = 0;
    for (int i = 0; i < MAX_ANIMS; i++) {
        if (_anims[i].age > most_aged) {
            most_aged = _anims[i].age;
            oldest = i;
        }
    }
    _anims[oldest] = {type, static_cast<int16_t>(px),
                      static_cast<int16_t>(py), 0, duration, true};
}

void Renderer::receive_events(const Event* events, int count, const Chamber& ch) {
    int cell = Cfg::CELL_SIZE;
    int half = cell / 2;

    for (int i = 0; i < count; i++) {
        const Event& ev = events[i];
        int px, py;

        switch (ev.type) {
        case EVT_FOOD_TAPPED:
            px = ev.food_tapped.x * cell + half;
            py = ev.food_tapped.y * cell + half;
            _spawn_anim(ANIM_TAP_RING, px, py, 10);
            break;

        case EVT_FOOD_DELIVERED:
            px = ev.food_delivered.x * cell + half;
            py = ev.food_delivered.y * cell + half;
            _spawn_anim(ANIM_FOOD_DELIVER, px, py, 8);
            break;

        case EVT_QUEEN_LAID_EGG:
            px = ev.position.x * cell + half;
            py = ev.position.y * cell + half;
            _spawn_anim(ANIM_EGG_LAID, px, py, 8);
            break;

        case EVT_YOUNG_HATCHED:
            if (ev.young_hatched.stage_to == 0xFF) {
                px = ev.young_hatched.x * cell + half;
                py = ev.young_hatched.y * cell + half;
                _spawn_anim(ANIM_HATCH, px, py, 12);
            }
            break;

        case EVT_YOUNG_DIED:
            px = ev.position.x * cell + half;
            py = ev.position.y * cell + half;
            _spawn_anim(ANIM_DEATH_YOUNG, px, py, 8);
            break;

        case EVT_LIL_GUY_DIED:
            px = ev.position.x * cell + half;
            py = ev.position.y * cell + half;
            _spawn_anim(ANIM_DEATH_WORKER, px, py, 10);
            break;

        default:
            break;
        }
    }
}

void Renderer::_draw_anims() {
    int write = 0;
    for (int i = 0; i < _anim_count; i++) {
        Anim& a = _anims[i];
        if (!a.active) continue;
        _draw_one_anim(a);
        a.age++;
        if (a.age >= a.duration) {
            a.active = false;
            continue;
        }
        if (write != i) _anims[write] = _anims[i];
        write++;
    }
    _anim_count = write;
}

void Renderer::_draw_one_anim(const Anim& a) {
    float t = static_cast<float>(a.age) / a.duration;

    switch (a.type) {
    case ANIM_TAP_RING: {
        int r = 4 + static_cast<int>(14 * t);
        uint16_t col = (t < 0.7f) ? _pal_glow_amber : _rgb565(
            (_cpal.floor1_r + _cpal.floor2_r) / 2,
            (_cpal.floor1_g + _cpal.floor2_g) / 2,
            (_cpal.floor1_b + _cpal.floor2_b) / 2);
        _gfx->drawCircle(a.px, a.py, r, col);
        if (r > 2) _gfx->drawCircle(a.px, a.py, r - 1, col);
        _mark_dirty(a.px - r - 1, a.py - r - 1, r * 2 + 3, r * 2 + 3);
        break;
    }

    case ANIM_FOOD_DELIVER: {
        int dy = static_cast<int>(6 * t);
        int y = a.py - dy;
        uint16_t col = (t < 0.5f) ? _pal_food_carry : _pal_food_light;
        _gfx->fillRect(a.px - 1, y - 1, 3, 3, col);
        _mark_dirty(a.px - 1, y - 2, 3, 4);
        break;
    }

    case ANIM_EGG_LAID: {
        int r = 3 + static_cast<int>(4 * t);
        uint16_t col = (t < 0.5f) ? _pal_egg_colour : _pal_larva_colour;
        _gfx->drawCircle(a.px, a.py, r, col);
        _mark_dirty(a.px - r - 1, a.py - r - 1, r * 2 + 3, r * 2 + 3);
        break;
    }

    case ANIM_HATCH: {
        int arm = 2 + static_cast<int>(8 * t);
        uint16_t col = (t < 0.6f) ? _pal_glow_amber : _pal_glow_warm;
        _gfx->drawFastHLine(a.px - arm, a.py, arm * 2 + 1, col);
        _gfx->drawFastVLine(a.px, a.py - arm, arm * 2 + 1, col);
        if (arm > 3) {
            int d = arm * 7 / 10;
            _gfx->drawPixel(a.px - d, a.py - d, col);
            _gfx->drawPixel(a.px + d, a.py - d, col);
            _gfx->drawPixel(a.px - d, a.py + d, col);
            _gfx->drawPixel(a.px + d, a.py + d, col);
        }
        _mark_dirty(a.px - arm - 1, a.py - arm - 1, arm * 2 + 3, arm * 2 + 3);
        break;
    }

    case ANIM_DEATH_WORKER: {
        int arm = (t < 0.3f) ? 3 : (4 - static_cast<int>(2 * t));
        if (arm < 1) arm = 1;
        uint16_t col = (t < 0.5f) ? _pal_ui_alert : _pal_ui_dim;
        for (int i = -arm; i <= arm; i++) {
            int px1 = a.px + i, py1 = a.py + i;
            int py2 = a.py - i;
            if (px1 >= 0 && px1 < SCREEN_W) {
                if (py1 >= 0 && py1 < SCREEN_H) _gfx->drawPixel(px1, py1, col);
                if (py2 >= 0 && py2 < SCREEN_H) _gfx->drawPixel(px1, py2, col);
            }
        }
        _mark_dirty(a.px - arm - 1, a.py - arm - 1, arm * 2 + 3, arm * 2 + 3);
        break;
    }

    case ANIM_DEATH_YOUNG: {
        int r = (t < 0.3f) ? 3 : static_cast<int>(3 * (1.0f - t));
        if (r < 1) r = 1;
        uint16_t col = _pal_ui_dim;
        _gfx->drawCircle(a.px, a.py, r, col);
        _mark_dirty(a.px - r - 1, a.py - r - 1, r * 2 + 3, r * 2 + 3);
        break;
    }
    }
}
