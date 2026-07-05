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
#include "weather.h"
#include "world_condition.h"
#include "rng.h"
#include <pgmspace.h>
#include <Preferences.h>
#include <algorithm>
#include <atomic>
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

// ---- Floor tint (user-set ground colour, per module) ----
// Brightness-neutral hue scaling applied after day/night interpolation,
// so the tinted floor still lives through the full lighting cycle.
static bool  _tint_active = false;
static float _tint_sr = 1.0f, _tint_sg = 1.0f, _tint_sb = 1.0f;
static uint8_t _tint_r = 0, _tint_g = 0, _tint_b = 0;
static volatile bool _tint_changed = false;

static inline uint8_t _tint_ch(uint8_t c, float s, float strength) {
    float v = c * (1.0f + (s - 1.0f) * strength);
    return v > 255.0f ? 255 : (uint8_t)v;
}

static void _apply_floor_tint() {
    if (!_tint_active) return;
    _cpal.floor1_r = _tint_ch(_cpal.floor1_r, _tint_sr, 0.85f);
    _cpal.floor1_g = _tint_ch(_cpal.floor1_g, _tint_sg, 0.85f);
    _cpal.floor1_b = _tint_ch(_cpal.floor1_b, _tint_sb, 0.85f);
    _cpal.floor2_r = _tint_ch(_cpal.floor2_r, _tint_sr, 0.85f);
    _cpal.floor2_g = _tint_ch(_cpal.floor2_g, _tint_sg, 0.85f);
    _cpal.floor2_b = _tint_ch(_cpal.floor2_b, _tint_sb, 0.85f);
    _cpal.grain_r  = _tint_ch(_cpal.grain_r,  _tint_sr, 0.60f);
    _cpal.grain_g  = _tint_ch(_cpal.grain_g,  _tint_sg, 0.60f);
    _cpal.grain_b  = _tint_ch(_cpal.grain_b,  _tint_sb, 0.60f);
    _cpal.outer_r  = _tint_ch(_cpal.outer_r,  _tint_sr, 0.40f);
    _cpal.outer_g  = _tint_ch(_cpal.outer_g,  _tint_sg, 0.40f);
    _cpal.outer_b  = _tint_ch(_cpal.outer_b,  _tint_sb, 0.40f);
}

void renderer_set_floor_tint(uint8_t r, uint8_t g, uint8_t b, bool persist) {
    _tint_r = r; _tint_g = g; _tint_b = b;
    _tint_active = (r | g | b) != 0;
    if (_tint_active) {
        float luma = 0.299f * r + 0.587f * g + 0.114f * b;
        if (luma < 1.0f) luma = 1.0f;
        _tint_sr = r / luma; _tint_sg = g / luma; _tint_sb = b / luma;
    } else {
        _tint_sr = _tint_sg = _tint_sb = 1.0f;
    }
    _tint_changed = true;
    if (persist) {
        Preferences prefs;
        prefs.begin("hive", false);
        prefs.putUInt("tint", ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        prefs.end();
    }
    Serial.printf("[renderer] floor tint %s (%d,%d,%d)\r\n",
                  _tint_active ? "set" : "off", r, g, b);
}

void renderer_load_floor_tint() {
    Preferences prefs;
    prefs.begin("hive", true);
    uint32_t v = prefs.getUInt("tint", 0);
    prefs.end();
    if (v) renderer_set_floor_tint((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, false);
}

uint32_t renderer_get_floor_tint() {
    return ((uint32_t)_tint_r << 16) | ((uint32_t)_tint_g << 8) | _tint_b;
}

static void _interpolate_chamber_palette() {
    float nf = g_tod.night_factor;

    const ChamberPalette* from;
    const ChamberPalette* to;
    float t;

    if (nf < 0.05f) {
        _cpal = CPAL_DAY;
        _apply_floor_tint();
        return;
    } else if (nf > 0.85f) {
        _cpal = CPAL_NIGHT;
        _apply_floor_tint();
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
    _apply_floor_tint();
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
// Queen at 2.0x (88px) is the 1.00 anchor
static constexpr float SCALE_QUEEN          = 2.0f;   // 44x44 base → 88px
static constexpr float SCALE_EGG            = 4.5f;   //  4x4  base → 18px (Amber: eggs should pop)
static constexpr float SCALE_SEED           = 4.4f;   //  6x6  base → 26px (was LARVA)
static constexpr float SCALE_HUSK           = 3.2f;   //  8x8  base → 26px (uses old pupa sprite)
static constexpr float SCALE_FOOD_PILE      = 1.5f;   // 12x8  base → 18x12
// Legacy aliases
static constexpr float SCALE_LARVA          = SCALE_SEED;
static constexpr float SCALE_PUPA           = SCALE_HUSK;

// ---- Sprite frame lookup ----
// Returns sprite data for a given role + animation frame.
// Returns nullptr if no dedicated frame exists — caller falls back to BASE.
struct SpriteRef {
    const uint16_t* data;
    int w, h;
};

static const SpriteRef* _get_worker_sprite(ConkerSpriteFrame frame) {
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
static unsigned long _prof_flush_total = 0;    // worker-side QSPI push time
static unsigned long _prof_wait_total = 0;     // core-1 time blocked on the worker
static unsigned long _prof_flush_pixels_total = 0;
static int           _prof_full_redraw_count = 0;
static int           _prof_frame_count = 0;
static unsigned long _prof_last_report = 0;
#endif

// ================================================================
//  Async flush worker (core 0) + double buffering
// ================================================================
// The full-frame QSPI push costs ~35ms — the single biggest CPU item on
// core 1 (v208 [perf]: 21ms render + 35ms flush per ~14fps frame). The
// worker runs the push on core 0; double buffering lets core 1 render
// frame N+1 into the other canvas while frame N is still on the wire
// (single-buffer v209 measured avg_wait=23ms — render and push serialise
// without the second buffer).
//
// The worker ONLY ever calls Arduino_Canvas::flush() — the same call the
// synchronous path used for 200 versions. v209 pushed the raw framebuffer
// with logical (480x320) dims; the panel is natively 320x480 (rotation is
// canvas-side) and the clip path sheared the image. Never touch raw dims.
//
// Safety invariants:
//  - A canvas is never WRITTEN while the worker reads it: posts alternate
//    A,B,A,B and draw() blocks until at most the MOST RECENT post is still
//    in flight — the canvas it is about to write is therefore idle.
//  - Double buffering requires the floor cache (every frame repaints in
//    full). The uncached floor path restores only dirty rects and relies
//    on the previous frame's pixels, so it forces single-buffer mode.
//  - Modal paths that paint outside the frame loop (OTA splash) drain to
//    zero in-flight pushes first, then own the display until reboot.
// Same shape as the v206 chores worker: core 0, priority 1, synchronous
// fallback if anything fails to allocate.
static TaskHandle_t      _flushw_task = nullptr;
static QueueHandle_t     _flushw_q    = nullptr;   // canvases queued to push
static SemaphoreHandle_t _flushw_done = nullptr;   // signalled per completed push
static std::atomic<int>  _flushw_inflight{0};
static Arduino_Canvas*   _canvas_a = nullptr;
static Arduino_Canvas*   _canvas_b = nullptr;      // null = single-buffer mode
// True while core 1 may write the current canvas (between draw()'s gate
// and flush()'s post). Only toggled from core 1.
static bool _flushw_owned = true;

static void _flushw_worker(void*) {
    Arduino_Canvas* c;
    for (;;) {
        xQueueReceive(_flushw_q, &c, portMAX_DELAY);
        unsigned long t0 = millis();
        c->flush();
#if RENDERER_PROFILE
        _prof_flush_total += millis() - t0;
#else
        (void)t0;
#endif
        _flushw_inflight.fetch_sub(1);
        xSemaphoreGive(_flushw_done);
        // MANDATORY yield: double-buffered, frames arrive faster than they
        // push, so this loop otherwise never blocks — a priority-1 task that
        // never blocks starves IDLE0 and the task watchdog reboots the board
        // (v210 crash-looped at ~25s uptime on exactly this). 1 tick = ~1ms:
        // IDLE0 gets a slice every frame, costs ~3% flush throughput.
        vTaskDelay(1);
    }
}

// Block until at most max_inflight pushes remain pending. The done
// semaphore is just a doze between checks; the atomic is the truth.
static void _flushw_wait(int max_inflight) {
    while (_flushw_inflight.load() > max_inflight)
        xSemaphoreTake(_flushw_done, pdMS_TO_TICKS(50));
}

void renderer_flush_drain() {
    if (!_flushw_task) return;
    _flushw_wait(0);
    _flushw_owned = true;
}

// ================================================================
//  Init
// ================================================================

void Renderer::init(Arduino_Canvas* canvas, Arduino_Canvas* canvas2,
                    Arduino_TFT* output) {
    _gfx = canvas;
    _output = output;
    _needs_full_redraw = true;
    _dirty_count = 0;
    _anim_count = 0;

    // Spin up the async flush worker (see block above). On any failure the
    // handles stay null and flush() falls back to the old synchronous push.
    _canvas_a = canvas;
    _canvas_b = canvas2;   // may be null; double-buffer gate happens below
    _flushw_q    = xQueueCreate(2, sizeof(Arduino_Canvas*));
    _flushw_done = xSemaphoreCreateBinary();
    if (!_flushw_q || !_flushw_done
            || xTaskCreatePinnedToCore(_flushw_worker, "fbflush", 4096, nullptr,
                                       1, &_flushw_task, 0) != pdPASS) {
        _flushw_task = nullptr;
        _canvas_b = nullptr;
        Serial.println("[renderer] flush worker unavailable — synchronous flush");
    }

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
        Serial.printf("[renderer] Floor cache allocated: %d bytes in PSRAM\r\n",
                      SCREEN_W * SCREEN_H * 2);
        _floor_cache_valid = false;
        _floor_cache_nf = -1.0f;
    } else {
        Serial.println("[renderer] WARNING: PSRAM alloc failed, floor cache disabled");
    }
#endif

    // Double buffering needs the floor cache: with it, every frame repaints
    // the full canvas, so alternating canvases is invisible. Without it the
    // dirty-rect floor restore reads the PREVIOUS frame's pixels — which
    // with two buffers would be the frame before last. Fall back to
    // single-buffer async in that case.
    bool floor_cached = false;
#if CHAMBER_FLOOR_CACHED
    floor_cached = (_floor_cache != nullptr);
#endif
    if (_canvas_b && !floor_cached) {
        _canvas_b = nullptr;
        Serial.println("[renderer] no floor cache — single-buffer flush");
    }
    Serial.printf("[renderer] flush: %s\r\n",
        !_flushw_task ? "synchronous"
                      : (_canvas_b ? "async double-buffer" : "async single-buffer"));
}

void Renderer::_union_dirty_into_bounds(FlushBounds& bounds) {
    for (int i = 0; i < _dirty_count; i++) {
        auto& d = _dirty[i];
        int16_t dx1 = d.x + d.w;
        int16_t dy1 = d.y + d.h;
        if (!bounds.any) {
            bounds.x0 = d.x; bounds.y0 = d.y;
            bounds.x1 = dx1; bounds.y1 = dy1;
            bounds.any = true;
        } else {
            if (d.x < bounds.x0) bounds.x0 = d.x;
            if (d.y < bounds.y0) bounds.y0 = d.y;
            if (dx1 > bounds.x1) bounds.x1 = dx1;
            if (dy1 > bounds.y1) bounds.y1 = dy1;
        }
    }
    // Clamp to screen
    if (bounds.any) {
        if (bounds.x0 < 0) bounds.x0 = 0;
        if (bounds.y0 < 0) bounds.y0 = 0;
        if (bounds.x1 > SCREEN_W) bounds.x1 = SCREEN_W;
        if (bounds.y1 > SCREEN_H) bounds.y1 = SCREEN_H;
    }
}

void Renderer::_union_bounds(FlushBounds& dst, const FlushBounds& src) {
    if (!src.any) return;
    if (src.full) { dst.full = true; return; }
    if (!dst.any) {
        dst.x0 = src.x0; dst.y0 = src.y0;
        dst.x1 = src.x1; dst.y1 = src.y1;
        dst.any = true;
    } else {
        if (src.x0 < dst.x0) dst.x0 = src.x0;
        if (src.y0 < dst.y0) dst.y0 = src.y0;
        if (src.x1 > dst.x1) dst.x1 = src.x1;
        if (src.y1 > dst.y1) dst.y1 = src.y1;
    }
}

void Renderer::flush() {
    // Union current frame's dirty rects (new sprite positions) into flush bounds
    _union_dirty_into_bounds(_flush_bounds);

    // Full-frame flush — windowed sub-region flush proved unreliable with the
    // AXS15231B QSPI display (rotation/stride issues). With 16 scattered workers
    // + HUD, the dirty bounding box covers ~100% of the screen anyway, so the
    // windowed path provided no benefit. Keeping dirty-rect tracking infrastructure
    // for future use (clustered layouts, companion app rendering, fewer workers).
    //
    // The push itself runs on the core-0 worker; core 1 is free the moment
    // the request is posted. No canvas write may happen from here until
    // draw() re-acquires ownership.
    if (_flushw_task) {
        if (_flushw_owned) {
            Arduino_Canvas* posted = _gfx;
            _flushw_inflight.fetch_add(1);
            if (xQueueSend(_flushw_q, &posted, 0) == pdTRUE) {
                _flushw_owned = false;
                // Double-buffered: draw the next frame into the other canvas
                // while this one is on the wire.
                if (_canvas_b)
                    _gfx = (_gfx == _canvas_a) ? _canvas_b : _canvas_a;
            } else {
                _flushw_inflight.fetch_sub(1);  // queue full — drop this frame
            }
        }
        // !owned: push still in flight and nothing drew since — never fall
        // through to a synchronous push, it would race the worker on the bus.
    } else {
#if RENDERER_PROFILE
        unsigned long t0 = millis();
#endif
        _gfx->flush();  // no worker — old synchronous path
#if RENDERER_PROFILE
        _prof_flush_total += millis() - t0;
#endif
    }

#if RENDERER_PROFILE
    // Still compute flush bounds for diagnostic reporting
    _union_dirty_into_bounds(_flush_bounds);
    unsigned long flush_px = _flush_bounds.any ?
        (unsigned long)(_flush_bounds.x1 - _flush_bounds.x0) * (_flush_bounds.y1 - _flush_bounds.y0) :
        SCREEN_W * SCREEN_H;
    _prof_flush_pixels_total += flush_px;
    _prof_full_redraw_count++;
#endif

    // Save current bounds for next frame (so restored floor regions get flushed)
    _prev_flush_bounds = {};
    _union_dirty_into_bounds(_prev_flush_bounds);
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
    uint16_t floor2 = _rgb565(_cpal.floor2_r, _cpal.floor2_g, _cpal.floor2_b);

    for (int i = 0; i < _dirty_count; i++) {
        auto& d = _dirty[i];
        if (d.x < FLOOR_X || d.y < FLOOR_Y ||
            d.x + d.w > FLOOR_X + FLOOR_W || d.y + d.h > FLOOR_Y + FLOOR_H) {
            _gfx->fillRect(d.x, d.y, d.w, d.h, floor2);
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
    // Async-flush safety gate: the canvas we are about to write must not be
    // on the wire. Double-buffered, that means at most ONE push pending (the
    // most recent — always the OTHER canvas, posts alternate); single-
    // buffered it means none. Normally a no-op by the time a frame is due.
    if (_flushw_task && !_flushw_owned) {
#if RENDERER_PROFILE
        unsigned long wait_t0 = millis();
#endif
        _flushw_wait(_canvas_b ? 1 : 0);
        _flushw_owned = true;
#if RENDERER_PROFILE
        _prof_wait_total += millis() - wait_t0;
#endif
    }

    // Capture previous frame's dirty rects as flush bounds
    // (these regions were restored by floor blit but display still shows old sprites)
    _flush_bounds = {};
    _union_dirty_into_bounds(_flush_bounds);
    // Also carry over any previous full-flush flag
    _union_bounds(_flush_bounds, _prev_flush_bounds);

#if BOOT_SPLASH_ENABLED
    if (_boot_splash_active) {
        _flush_bounds.full = true;  // Boot splash = full redraw
        if (_tick_boot_splash(ch)) return;
    }
#endif

#if RENDERER_PROFILE
    unsigned long frame_start = millis();
#endif

    _update_night_palette();
    _interpolate_chamber_palette();

    // Tint changed (app command or serial) — rebuild everything once
    if (_tint_changed) {
        _tint_changed = false;
        _floor_cache_valid = false;
        _needs_full_redraw = true;
    }

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
            _flush_bounds.full = true;  // Palette change = full flush
        }

        _blit_floor_cache_full();
        _dirty_count = 0;
        _needs_full_redraw = false;
    } else
#endif
    {
        if (_needs_full_redraw) {
            uint16_t floor2 = _rgb565(_cpal.floor2_r, _cpal.floor2_g, _cpal.floor2_b);
            _gfx->fillScreen(floor2);
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

    // Layer 2: floor-level sprites (food, brood) — Y-sorted
    _build_floor_sprites(ch);
    _draw_sorted_sprites(_floor_sprites, _floor_sprite_count, ch);
    _draw_plants(ch);     // garden crops sit on the floor layer too
    _draw_artworks(ch);   // conker-made works, in their makers' colours

    // Layer 3: living agents (workers, queen) — Y-sorted, queen +2 cell bias
    _build_agent_sprites(ch, lerp_t);
    _draw_sorted_sprites(_agent_sprites, _agent_sprite_count, ch);

    _draw_tunnel_entrances(ch);
    if (debug_phero) { _draw_phero_overlay(ch); _flush_bounds.full = true; }
    _draw_anims();
    _draw_scent_marks(ch);
    _draw_fireflies(ch, lerp_t);
    _draw_critters(ch, lerp_t);
    _draw_weather();
    // Weather particles cover the whole screen — force full flush when active
    if (g_weather.valid && g_weather.condition >= WX_DRIZZLE)
        _flush_bounds.full = true;

    _tick_banner();  // story-beat narration, drawn on top of everything

    _frame++;

#if RENDERER_PROFILE
    _prof_sprites_total += millis() - sprites_start;
    _prof_frame_total += millis() - frame_start;
    _prof_frame_count++;

    unsigned long now = millis();
    if (now - _prof_last_report >= 30000 && _prof_frame_count > 0) {
        unsigned long avg_flush_px = _prof_flush_pixels_total / _prof_frame_count;
        int flush_pct = (int)(avg_flush_px * 100 / (SCREEN_W * SCREEN_H));
        int full_pct = _prof_full_redraw_count * 100 / _prof_frame_count;
        // avg_flush is worker-side (concurrent with render since v209);
        // avg_wait is what core 1 actually lost blocking on it. fps is
        // wall-clock (frames / report window) — the old render-time-only
        // fps read ~45 while the glass showed ~14.
        unsigned long window_ms = now - _prof_last_report;
        Serial.printf("[perf] frames=%d avg_total=%lums avg_floor=%lums avg_sprites=%lums avg_flush=%lums avg_wait=%lums flush=%d%% full=%d%% fps=%.1f\r\n",
            _prof_frame_count,
            _prof_frame_total / _prof_frame_count,
            _prof_floor_total / _prof_frame_count,
            _prof_sprites_total / _prof_frame_count,
            _prof_flush_total / _prof_frame_count,
            _prof_wait_total / _prof_frame_count,
            flush_pct, full_pct,
            1000.0f * _prof_frame_count / (window_ms > 0 ? window_ms : 1));
        _prof_frame_total = 0;
        _prof_floor_total = 0;
        _prof_sprites_total = 0;
        _prof_flush_total = 0;
        _prof_wait_total = 0;
        _prof_flush_pixels_total = 0;
        _prof_full_redraw_count = 0;
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

    uint16_t floor2_col = _rgb565(_cpal.floor2_r, _cpal.floor2_g, _cpal.floor2_b);

    // Fill entire screen with floor edge color so no dark frame shows
    // through physically rounded LCD corners
    _gfx->fillScreen(floor2_col);

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
    _gfx->fillRect(FLOOR_X, FLOOR_Y, FLOOR_W, FLOOR_H, floor2_col);

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

void Renderer::set_milestone_decor(uint8_t bits) {
    if (bits == _milestone_decor) return;
    _milestone_decor = bits;
    _floor_cache_valid = false;   // decor lives in the cached floor
    _needs_full_redraw = true;
}

// Colony achievements as permanent chamber objects — visible, accumulating
// history on the glass ("even tamagotchi had growth" — now the room itself
// grows). Drawn into the floor cache alongside the edge pebbles.
void Renderer::_draw_milestone_decor() {
    // Mossy stone — 25 workers born: the colony has real history now
    if (_milestone_decor & 0x01) {
        int px = SCREEN_W - 120, py = 36;
        uint8_t pr = _lerp8(150, 85, _nf), pg = _lerp8(148, 84, _nf), pb = _lerp8(140, 90, _nf);
        uint16_t stone = _rgb565(pr, pg, pb);
        uint16_t moss  = _rgb565(_lerp8(90, 40, _nf), _lerp8(150, 80, _nf), _lerp8(70, 45, _nf));
        _gfx->fillRoundRect(px - 6, py - 4, 12, 8, 3, stone);
        _gfx->drawFastHLine(px - 5, py + 4, 10, _rgb565(pr * 3 / 4, pg * 3 / 4, pb * 3 / 4));
        _gfx->drawFastHLine(px - 4, py - 4, 7, moss);   // moss cap
        _gfx->drawPixel(px - 5, py - 3, moss);
        _gfx->drawPixel(px + 3, py - 3, moss);
    }
    // Cairn — the colony weathered its first challenge together
    if (_milestone_decor & 0x02) {
        int px = 120, py = SCREEN_H - 28;
        uint8_t pr = _lerp8(160, 90, _nf), pg = _lerp8(155, 88, _nf), pb = _lerp8(148, 95, _nf);
        uint16_t c  = _rgb565(pr, pg, pb);
        uint16_t cd = _rgb565(pr * 3 / 4, pg * 3 / 4, pb * 3 / 4);
        _gfx->fillRoundRect(px - 5, py,     10, 4, 2, c);    // base
        _gfx->fillRoundRect(px - 3, py - 3,  7, 3, 1, cd);   // middle
        _gfx->fillRoundRect(px - 1, py - 5,  4, 2, 1, c);    // top
    }
    // Wildflower — the first best friendship blossomed here
    if (_milestone_decor & 0x04) {
        int px = 90, py = 30;
        uint16_t stem  = _rgb565(_lerp8(95, 45, _nf), _lerp8(150, 85, _nf), _lerp8(75, 50, _nf));
        uint16_t petal = _rgb565(_lerp8(245, 140, _nf), _lerp8(170, 100, _nf), _lerp8(200, 130, _nf));
        uint16_t heart = _rgb565(_lerp8(250, 150, _nf), _lerp8(220, 130, _nf), _lerp8(120, 80, _nf));
        _gfx->drawFastVLine(px, py, 5, stem);
        _gfx->drawPixel(px - 1, py - 1, petal);
        _gfx->drawPixel(px + 1, py - 1, petal);
        _gfx->drawPixel(px, py - 2, petal);
        _gfx->drawPixel(px, py, heart);
    }
    // Golden seed — a Bug Hunter has been crowned
    if (_milestone_decor & 0x08) {
        int px = SCREEN_W - 26, py = SCREEN_H / 2 - 20;
        uint16_t gold  = _rgb565(_lerp8(230, 140, _nf), _lerp8(185, 110, _nf), _lerp8(70, 45, _nf));
        uint16_t glint = _rgb565(_lerp8(255, 170, _nf), _lerp8(235, 150, _nf), _lerp8(160, 90, _nf));
        _gfx->fillRoundRect(px - 3, py - 2, 7, 5, 2, gold);
        _gfx->drawPixel(px - 1, py - 1, glint);
        _gfx->drawPixel(px, py - 2, glint);
    }
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

    // Earned milestone objects (drawn last, on top of the ambient decor)
    _draw_milestone_decor();
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
    return a.render_x < b.render_x;  // stable tiebreaker (x doesn't shuffle between frames)
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
        sd.size_px  = static_cast<uint16_t>(FOOD_PILE_W * SCALE_FOOD_PILE + 0.5f);
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
            case STAGE_SEED:
                sd.kind = SK_SEED;
                sd.size_px = static_cast<uint16_t>(LARVA_W * SCALE_SEED + 0.5f);
                break;
            default: _floor_sprite_count--; break;
        }
    }

    // Husks (static, drawn underneath workers)
    for (int i = 0; i < ch.husk_count && _floor_sprite_count < MAX_FLOOR_SPRITES; i++) {
        auto& h = ch.husks[i];
        auto& sd = _floor_sprites[_floor_sprite_count++];
        sd.render_x = h.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.render_y = h.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        sd.sort_y   = sd.render_y;
        sd.kind     = SK_HUSK;
        sd.size_px  = static_cast<uint16_t>(PUPA_W * h.scale_factor * 0.3f + 0.5f);
        sd.flags    = 0;
        sd.entity_idx = i;
    }

    std::stable_sort(_floor_sprites, _floor_sprites + _floor_sprite_count, _sprite_cmp);
}

void Renderer::_build_agent_sprites(const Chamber& ch, float lerp_t) {
    _agent_sprite_count = 0;
    float t = (lerp_t < 0.0f) ? 0.0f : ((lerp_t > 1.0f) ? 1.0f : lerp_t);

    // Workers
    for (int i = 0; i < ch.conker_count; i++) {
        auto& w = ch.conkers[i];
        if (!w.alive || w.departing) continue;

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
            // Hop-bounded: offset only decreases so it can never terminate the
            // loop on its own; a stack_on cycle here would hang the render path
            while (cur >= 0 && cur < ch.conker_count
                   && stack_depth < Cfg::MAX_CONKERS) {
                auto& b = ch.conkers[cur];
                float s = b.render_scale();
                offset -= static_cast<int>(WORKER_PIONEER_H * s + 0.5f) * 0.6f;
                stack_depth++;
                if (b.stack_on < 0) base_idx = cur;  // ground-level ant
                cur = b.stack_on;
            }
            // Sort by base ant's position so whole tower moves as one z-unit
            auto& base = ch.conkers[base_idx];
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
                    height -= static_cast<int>(WORKER_PIONEER_H * w.render_scale() + 0.5f) * 0.6f;
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
        } else if (w.anim_type == LG_ANIM_NOTICE) {
            // Startle hop: noticed the giant outside the glass
            float p = 1.0f - static_cast<float>(w.anim_remaining_ticks) / 12.0f;
            py -= static_cast<int>(sinf(p * 3.14159f) * 5.0f);
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

                // Carry waddle: render-only sway perpendicular to travel,
                // heavier loads sway more. Cell path (and the laid trail)
                // is untouched — this never alters sim position.
                if (w.state == STATE_TO_HOME && w.food_carried > 0) {
                    float load = (w.carry_amount > 0.0f)
                               ? w.food_carried / w.carry_amount : 1.0f;
                    if (load > 1.0f) load = 1.0f;
                    float amp = Cfg::CARRY_WADDLE_AMP_PX * (0.5f + 0.5f * load);
                    float wphase = (fx + fy) * Cfg::CARRY_WADDLE_FREQ + i * 0.7f;
                    float sway = sinf(wphase * 6.28318f) * amp;
                    px += static_cast<int>(-w.facing_dy * sway);
                    py += static_cast<int>( w.facing_dx * sway);
                }
            }
        }

        float scale = w.render_scale();

        auto& sd = _agent_sprites[_agent_sprite_count++];
        sd.sort_y    = sort_y_stable;
        sd.render_x  = px;
        sd.render_y  = py;
        sd.size_px   = static_cast<uint16_t>(WORKER_PIONEER_W * scale + 0.5f);
        sd.kind      = SK_WORKER;
        sd.flags     = (w.facing_dx < -0.1f) ? 1 : 0;
        sd.entity_idx = i;
        sd.tint_seed = w.tint_seed;
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
        sd.tint_seed = 0;
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
            _draw_sprite_scaled(sd.render_x, sd.render_y,
                                FOOD_PILE, FOOD_PILE_W, FOOD_PILE_H,
                                SCALE_FOOD_PILE);
            break;
        }
        case SK_EGG: {
            // Soft ring under each egg so the clutch pops against the floor
            int r = sd.size_px / 2 + 2;
            _mark_dirty(sd.render_x - r - 1, sd.render_y - r - 1, 2 * r + 3, 2 * r + 3);
            _gfx->drawCircle(sd.render_x, sd.render_y, r, _pal_egg_colour);
            _draw_sprite_scaled(sd.render_x, sd.render_y, EGG, EGG_W, EGG_H, SCALE_EGG);
            break;
        }
        case SK_SEED:
            _draw_sprite_scaled(sd.render_x, sd.render_y, LARVA, LARVA_W, LARVA_H, SCALE_SEED);
            break;
        case SK_HUSK: {
            // Husks use old pupa sprite, desaturated, at the dead conker's scale
            float husk_scale = 1.0f;
            if (sd.entity_idx >= 0 && sd.entity_idx < ch.husk_count)
                husk_scale = ch.husks[sd.entity_idx].scale_factor * 0.3f;
            _draw_sprite_scaled_tinted(sd.render_x, sd.render_y, PUPA, PUPA_W, PUPA_H,
                                       husk_scale, false, 0, 0.70f);  // heavy grey desaturation
            break;
        }
        case SK_QUEEN:
            _draw_sprite_scaled(sd.render_x, sd.render_y, QUEEN, QUEEN_W, QUEEN_H, SCALE_QUEEN);
            break;
        case SK_WORKER: {
            auto& w = ch.conkers[sd.entity_idx];
            float scale = w.render_scale();

            // Sprite frame lookup: use dedicated frame if available, else base
            ConkerSpriteFrame frame = LG_FRAME_BASE;
            if (w.anim_type == LG_ANIM_GROOMING) frame = LG_FRAME_LEAN;
            else if (w.anim_type == LG_ANIM_SNOOZE) frame = LG_FRAME_SNOOZE;
            else if (w.state == STATE_MOURNING) frame = LG_FRAME_LEAN;  // head bowed at the vigil
            const SpriteRef* spr = _get_worker_sprite(frame);
            if (!spr) spr = _get_worker_sprite(LG_FRAME_BASE);

            // Visual ageing: desaturate as conker ages
            // 0-50% life: normal, 50-100%: ramp to 70% desaturation
            float age_grey = 0.0f;
            if (w.lifespan_ms > 0) {
                float age_frac = static_cast<float>(w.lived_ms) / w.lifespan_ms;
                if (age_frac > 0.5f) {
                    age_grey = (age_frac - 0.5f) * 2.0f * 0.70f;
                    if (age_grey > 0.70f) age_grey = 0.70f;
                }
            }

            _draw_sprite_scaled_tinted(sd.render_x, sd.render_y, spr->data,
                                spr->w, spr->h, scale, flip, sd.tint_seed, age_grey);

            // Food-share receiver pulse: warm dot during first half of animation
            if (w.anim_type == LG_ANIM_FOOD_SHARE_RECEIVER
                && w.anim_remaining_ticks > Cfg::FOOD_SHARE_DURATION_TICKS / 2) {
                _gfx->fillRect(sd.render_x - 1, sd.render_y - 1, 3, 3, _pal_food_carry);
                _mark_dirty(sd.render_x - 1, sd.render_y - 1, 3, 3);
            }

            if (w.food_carried > 0) {
                // Load-scaled bundle on their back — a full carrier visibly
                // hauls more than one with a nibble (placeholder until the
                // artist's LG_FRAME_CARRYING lands)
                float load = (w.carry_amount > 0.0f)
                                 ? w.food_carried / w.carry_amount : 1.0f;
                if (load > 1.0f) load = 1.0f;
                int fs = 2 + static_cast<int>(3.0f * load);  // 2..5 px
                int mx = sd.render_x - static_cast<int>(w.facing_dx * 4);
                int my = sd.render_y - static_cast<int>(w.facing_dy * 4);
                _gfx->fillRect(mx - fs / 2, my - fs / 2, fs, fs, _pal_food_carry);
                _mark_dirty(mx - fs / 2, my - fs / 2, fs, fs);
            }

            // Worn keepsake — a gift from a best friend, worn for life.
            // Sized with the wearer: fixed-pixel keepsakes vanished on a
            // grown conker (a 5px hat on a ~25px body read as noise).
            if (w.accessory != 0) {
                float rs = w.render_scale();
                if (w.accessory == 1) {          // petal hat
                    // Wide-brimmed and proud — spans most of the head, with
                    // thickness that scales too (a fixed 2px brim read as a
                    // sliver on a grown conker).
                    int hw = (int)(rs * 3.2f); if (hw < 4) hw = 4;   // brim half-width
                    int bt = (int)(rs * 0.9f); if (bt < 2) bt = 2;   // brim thickness
                    int ct = bt + 1;                                  // crown height
                    int hy = sd.render_y - (int)(rs * 4.5f);          // brim top row
                    _gfx->fillRect(sd.render_x - hw, hy, hw * 2 + 1, bt,
                                   _rgb565(240, 160, 190));            // brim
                    _gfx->fillRect(sd.render_x - hw / 2, hy - ct, hw + 1, ct,
                                   _rgb565(250, 195, 215));            // crown petals
                    _gfx->fillRect(sd.render_x - hw, hy - 1, 2, 1,
                                   _rgb565(250, 195, 215));            // petal fringe L
                    _gfx->fillRect(sd.render_x + hw - 1, hy - 1, 2, 1,
                                   _rgb565(250, 195, 215));            // petal fringe R
                    _gfx->fillRect(sd.render_x - 1, hy - ct - 2, 2, 2,
                                   _rgb565(255, 225, 150));            // golden bud
                    _mark_dirty(sd.render_x - hw - 1, hy - ct - 3,
                                hw * 2 + 3, ct + bt + 4);
                } else if (w.accessory == 2) {   // seed pendant
                    int ps = (int)(rs * 1.2f); if (ps < 3) ps = 3;
                    int py2 = sd.render_y + (int)(rs * 1.0f);
                    _gfx->fillRect(sd.render_x - ps / 2, py2, ps, ps,
                                   _rgb565(210, 165, 70));
                    _gfx->fillRect(sd.render_x - ps / 2, py2, ps, 1,
                                   _rgb565(245, 210, 120));            // catch the light
                    _mark_dirty(sd.render_x - ps / 2 - 1, py2 - 1, ps + 2, ps + 2);
                } else {                          // grass band
                    // Worn as a woven sash across the middle — the old 2px
                    // vertical tick vanished against the tinted body. Bright
                    // core with a sunlit top edge and dark under-edge so it
                    // reads on any body colour, knot and blade at the hip.
                    int bw = (int)(rs * 4.0f); if (bw < 4) bw = 4;    // sash half-width
                    int bt = (int)(rs * 0.8f); if (bt < 2) bt = 2;    // sash thickness
                    int by = sd.render_y + (int)(rs * 0.5f);          // waist row
                    _gfx->fillRect(sd.render_x - bw, by, bw * 2 + 1, bt,
                                   _rgb565(120, 205, 90));             // woven grass
                    _gfx->drawFastHLine(sd.render_x - bw, by - 1, bw * 2 + 1,
                                        _rgb565(195, 235, 140));       // sunlit edge
                    _gfx->drawFastHLine(sd.render_x - bw, by + bt, bw * 2 + 1,
                                        _rgb565(45, 110, 40));         // shaded edge
                    int kx = sd.render_x + bw - 1;                     // knot at the hip
                    _gfx->fillRect(kx - 1, by - 1, 3, bt + 2, _rgb565(80, 160, 60));
                    _gfx->drawFastVLine(kx, by - bt - 2, bt + 1,
                                        _rgb565(150, 220, 110));       // blade poking up
                    _mark_dirty(sd.render_x - bw - 1, by - bt - 3,
                                bw * 2 + 4, bt * 2 + 6);
                }
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

            // Mood emote — the loudest unmet need, floating above the head.
            // Sleep has its own Zs; stacked riders are too cramped to read;
            // mourners get the grief tear below instead.
            if (w.mood != MOOD_CONTENT && w.anim_type != LG_ANIM_SNOOZE
                    && w.stack_on < 0 && w.state != STATE_MOURNING) {
                const char* glyph = nullptr;
                uint16_t mc = 0;
                switch (w.mood) {
                    case MOOD_PLAYING:  glyph = "\x0E"; mc = _rgb565(255, 220, 90);  break; // music note
                    case MOOD_BORED:    glyph = "...";  mc = _rgb565(150, 150, 165); break;
                    case MOOD_RESTLESS: glyph = "?";    mc = _rgb565(220, 185, 95);  break;
                    case MOOD_SLEEPY:   glyph = "z";    mc = _rgb565(170, 170, 215); break; // pre-bed drowse
                    case MOOD_HAPPY:    glyph = "\x03"; mc = _rgb565(255, 140, 160); break; // afterglow heart
                    default: break;
                }
                if (glyph) {
                    int ex = sd.render_x + 4;
                    int ey = sd.render_y - 12;
                    _gfx->setTextSize(1);
                    _gfx->setTextColor(mc);
                    _gfx->setCursor(ex, ey);
                    _gfx->print(glyph);
                    _mark_dirty(ex, ey, 20, 8);
                }
                // Lonely: a little gloom cloud, slowly bobbing — no font
                // glyph reads as "missing company", so it's drawn by hand
                if (w.mood == MOOD_LONELY) {
                    int bob = (_frame / 12) % 2;
                    int cxp = sd.render_x + 5;
                    int cyp = sd.render_y - 13 + bob;
                    uint16_t cc = _rgb565(120, 130, 160);
                    _gfx->fillCircle(cxp - 3, cyp + 1, 2, cc);
                    _gfx->fillCircle(cxp,     cyp,     3, cc);
                    _gfx->fillCircle(cxp + 3, cyp + 1, 2, cc);
                    _mark_dirty(cxp - 6, cyp - 4, 13, 9);
                }
            }

            // Followed star — a small gold diamond so a favourite stays
            // findable at a glance (fed by the app's pin list)
            if (ch.is_followed(w.id) && w.stack_on < 0) {
                int fx = sd.render_x - 9;
                int fy = sd.render_y - 11;
                uint16_t gold = _rgb565(255, 215, 90);
                _gfx->drawPixel(fx, fy - 1, gold);
                _gfx->drawPixel(fx - 1, fy, gold);
                _gfx->drawPixel(fx, fy, gold);
                _gfx->drawPixel(fx + 1, fy, gold);
                _gfx->drawPixel(fx, fy + 1, gold);
                _mark_dirty(fx - 2, fy - 2, 5, 5);
            }

            // At work on a piece: little glints fly as the maker works
            if (w.state == STATE_CRAFTING && w.anim_type == LG_ANIM_GROOMING
                    && (_frame % 16) < 3) {
                uint16_t glint = _rgb565(255, 240, 200);
                int gx = sd.render_x + w.anim_lean_dx * 7 + ((_frame >> 2) & 1) * 2 - 1;
                int gy = sd.render_y + w.anim_lean_dy * 7 - ((_frame >> 3) & 1);
                _gfx->drawPixel(gx, gy, glint);
                _gfx->drawPixel(gx + 1, gy - 1, glint);
                _mark_dirty(gx - 1, gy - 2, 4, 4);
            }

            // Grief tear: a single slow tear while standing vigil
            if (w.state == STATE_MOURNING) {
                float tphase = (millis() % 2400) / 2400.0f;
                int ty = sd.render_y - 2 + static_cast<int>(tphase * 6);
                uint16_t tc = _rgb565(150, 190, 235);
                _gfx->drawPixel(sd.render_x + 4, ty, tc);
                _gfx->drawPixel(sd.render_x + 4, ty + 1, tc);
                _mark_dirty(sd.render_x + 4, sd.render_y - 2, 2, 9);
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

    _draw_sprite_scaled_tinted(cx, cy, data, sw, sh, scale, flip_h, 0);
}

// Eye-white sprite colours, exempt from the per-conker recolour (see loop below).
static const uint16_t EYE_HI = 0xE77A;   // upper eye highlight (cream)
static const uint16_t EYE_LO = 0xCF35;   // lower eye (pale)

// Fully-saturated RGB (0-255) for a hue angle in degrees — the vivid colour a
// rare conker recolours toward. (s=1, v=1 HSV → RGB.)
static void _hue_vivid(float h, int& r, int& g, int& b) {
    float hp = h / 60.0f;
    int   seg = (int)hp;
    float f   = hp - seg;
    float q = 1.0f - f, t = f;
    float rf, gf, bf;
    switch (seg % 6) {
        case 0:  rf = 1; gf = t; bf = 0; break;
        case 1:  rf = q; gf = 1; bf = 0; break;
        case 2:  rf = 0; gf = 1; bf = t; break;
        case 3:  rf = 0; gf = q; bf = 1; break;
        case 4:  rf = t; gf = 0; bf = 1; break;
        default: rf = 1; gf = 0; bf = q; break;
    }
    r = (int)(rf * 255.0f);
    g = (int)(gf * 255.0f);
    b = (int)(bf * 255.0f);
}

void Renderer::_draw_sprite_scaled_tinted(int cx, int cy, const uint16_t* data,
                                           int sw, int sh, float scale,
                                           bool flip_h, uint8_t tint_seed,
                                           float age_grey) {
    int dw = static_cast<int>(sw * scale + 0.5f);
    int dh = static_cast<int>(sh * scale + 0.5f);
    if (dw > MAX_SCALED_DIM) dw = MAX_SCALED_DIM;
    if (dh > MAX_SCALED_DIM) dh = MAX_SCALED_DIM;

    int ox = cx - dw / 2;
    int oy = cy - dh / 2;
    _mark_dirty(ox, oy, dw, dh);

    bool tint = (_nf > 0.01f);
    // Per-conker colour. Every conker gets its OWN vivid hue so the colony is easy
    // to tell apart; what's distributed like the size bell curve is how far a
    // conker is allowed to stray from the warm band. The common case is a warm hue
    // (reds/oranges/ambers/golds) at strong saturation; a rare roll widens the band
    // so the occasional outlier lands on a vivid off-hue (green/teal/violet).
    //
    // The sprite's luma drives shading (light = highlight, dark = shadow), but it's
    // a fairly dark brown, so we DON'T scale the target by it directly — that just
    // dims every conker. Instead we treat REF_LUMA as the sprite's body tone: a
    // pixel at REF maps to the FULL-brightness target colour, lighter pixels brighten
    // it (toward white highlights) and darker pixels shade it down. Keeps conkers
    // bright and vivid while preserving the sprite's modelling.
    static const int REF_LUMA = 98;   // lower = lighter conkers (body tone → target colour)
    int  tgt_r = 0, tgt_g = 0, tgt_b = 0;
    int  recolor = 0;     // 0..256 pull toward the target colour
    int  ref_eff = REF_LUMA;
    bool has_recolor = false;
    if (tint_seed != 0) {
        uint32_t hs = (uint32_t)tint_seed * 2654435761u;   // spread 8 bits → 32
        float ur = (hs & 0xFFFF) / 65535.0f;               // rarity roll
        float uh = ((hs >> 16) & 0xFFFF) / 65535.0f;       // hue position in the band
        // A decorrelated third roll for per-conker lightness, so two conkers that
        // land on the same hue still differ in shade (one deeper, one lighter).
        uint32_t h2 = hs ^ (hs >> 13);
        float ul = ((h2 >> 5) & 0xFF) / 255.0f;
        float rare = ur * ur; rare = rare * rare * rare;    // ur^6 — straying far is rare
        // ±85° common band (pinks through ambers to spring greens) — ±40°
        // made any two commons read as the same amber at arm's length.
        // Rare rolls still reach the whole wheel (max unchanged at ±240°).
        float spread = 85.0f + 155.0f * rare;
        float hue = 28.0f + (uh * 2.0f - 1.0f) * spread;    // anchor on orange (28°)
        while (hue < 0.0f)      hue += 360.0f;
        while (hue >= 360.0f)   hue -= 360.0f;
        _hue_vivid(hue, tgt_r, tgt_g, tgt_b);
        recolor = (int)((0.70f + 0.25f * rare) * 256.0f);   // 0.70x .. 0.95x
        // Base brightness 0.85x..1.20x; rare conkers that also roll bright get an
        // extra lift (up to ~1.75x) so the occasional outlier really pops, while a
        // rare one that rolls dark stays deep and moody.
        float light = 0.85f + 0.35f * ul + 0.55f * rare * ul;
        ref_eff = (int)(REF_LUMA / light);                  // lower ref = brighter conker
        if (ref_eff < 55) ref_eff = 55;
        has_recolor = true;
    }
    bool has_grey = (age_grey > 0.001f);
    uint16_t row_buf[MAX_SCALED_DIM];

    for (int dy = 0; dy < dh; dy++) {
        int py = oy + dy;
        if (py < 0 || py >= SCREEN_H) continue;
        int sy = dy * sh / dh;

        int dx_start = (ox < 0) ? -ox : 0;
        int dx_end   = (ox + dw > SCREEN_W) ? (SCREEN_W - ox) : dw;

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
            // Eye whites (two fixed sprite colours) are exempt from the per-conker
            // recolour — tinting them looks odd. They still dim with night.
            bool is_eye = (c == EYE_HI || c == EYE_LO);
            if (tint) c = tint_night(c, _nf, 0.4f);
            if (has_recolor && !is_eye) {
                int r8 = ((c >> 11) & 0x1F) << 3;
                int g8 = ((c >> 5)  & 0x3F) << 2;
                int b8 =  (c        & 0x1F) << 3;
                int luma = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
                // Shade the full-brightness target by the sprite's luma relative to
                // its body tone. shade=256 at REF_LUMA → full target colour; brighter
                // pixels push past it (clamp → white highlight), darker ones dim it.
                int shade = (luma << 8) / ref_eff;
                int tr = (tgt_r * shade) >> 8; if (tr > 255) tr = 255;
                int tg = (tgt_g * shade) >> 8; if (tg > 255) tg = 255;
                int tb = (tgt_b * shade) >> 8; if (tb > 255) tb = 255;
                // Pull the sprite pixel toward that bright target colour.
                r8 += ((tr - r8) * recolor) >> 8;
                g8 += ((tg - g8) * recolor) >> 8;
                b8 += ((tb - b8) * recolor) >> 8;
                if (r8 < 0) r8 = 0; if (r8 > 255) r8 = 255;
                if (g8 < 0) g8 = 0; if (g8 > 255) g8 = 255;
                if (b8 < 0) b8 = 0; if (b8 > 255) b8 = 255;
                c = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);
            }
            if (has_grey) {
                int r5 = (c >> 11) & 0x1F;
                int g6 = (c >> 5)  & 0x3F;
                int b5 =  c        & 0x1F;
                int grey5 = (r5 * 2 + (g6 >> 1) + b5 * 2) / 5;
                int grey6 = grey5 * 2;
                r5 = r5 + static_cast<int>((grey5 - r5) * age_grey);
                g6 = g6 + static_cast<int>((grey6 - g6) * age_grey);
                b5 = b5 + static_cast<int>((grey5 - b5) * age_grey);
                c = (r5 << 11) | (g6 << 5) | b5;
            }
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

        case EVT_CONKER_DIED:
            px = ev.position.x * cell + half;
            py = ev.position.y * cell + half;
            _spawn_anim(ANIM_DEATH_WORKER, px, py, 10);
            break;

        case EVT_BOND_FORMED:
        case EVT_BOND_MUTUAL: {
            // Resolve both ids to local, living conkers for the heart anim.
            // Non-local conkers just skip the animation — the banner (mutual
            // only) still reads from the names carried in the payload.
            int ax = -1, ay = 0, bx = -1, by = 0;
            for (int c = 0; c < ch.conker_count; c++) {
                const Conker& w = ch.conkers[c];
                if (!w.alive) continue;
                if (w.id == ev.bond.a_id) {
                    ax = static_cast<int>(w.x * cell);
                    ay = static_cast<int>(w.y * cell);
                } else if (w.id == ev.bond.b_id) {
                    bx = static_cast<int>(w.x * cell);
                    by = static_cast<int>(w.y * cell);
                }
            }
            bool big = (ev.type == EVT_BOND_MUTUAL);
            if (ax >= 0 && bx >= 0)
                _spawn_anim(ANIM_HEARTS, (ax + bx) / 2, (ay + by) / 2 - 6,
                            big ? 45 : 24);
            else if (ax >= 0)
                _spawn_anim(ANIM_HEARTS, ax, ay - 6, big ? 45 : 24);
            else if (bx >= 0)
                _spawn_anim(ANIM_HEARTS, bx, by - 6, big ? 45 : 24);
            if (big) {
                char msg[64];
                snprintf(msg, sizeof(msg), "\x03 %s & %s are best friends",
                         ev.bond.a_name, ev.bond.b_name);
                banner(msg);
            }
            break;
        }

        case EVT_TRAIT_EARNED: {
            for (int c = 0; c < ch.conker_count; c++) {
                const Conker& w = ch.conkers[c];
                if (w.alive && w.id == ev.trait.conker_id) {
                    _spawn_anim(ANIM_TRAIT_SPARKLE,
                                static_cast<int>(w.x * cell),
                                static_cast<int>(w.y * cell), 30);
                    break;
                }
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "\x0F %s earned '%s'",
                     ev.trait.who, _trait_label(ev.trait.trait_bit));
            banner(msg);
            break;
        }

        case EVT_MOURNING: {
            char msg[64];
            if (ev.mourning.dead_name[0])
                snprintf(msg, sizeof(msg), "%s stands vigil for %s",
                         ev.mourning.mourner_name, ev.mourning.dead_name);
            else
                snprintf(msg, sizeof(msg), "%s stands vigil for a friend",
                         ev.mourning.mourner_name);
            banner(msg);
            break;
        }

        case EVT_CHALLENGE_STARTED: {
            static const char* const START_MSGS[] = {
                "", "A heatwave bakes the colony!", "A cold snap grips the colony!",
                "A drought parches the colony!", "A storm batters the colony!",
            };
            if (ev.challenge.challenge_type < CHALLENGE_COUNT)
                banner(START_MSGS[ev.challenge.challenge_type]);
            break;
        }

        case EVT_CHALLENGE_ENDED: {
            static const char* const END_MSGS[] = {
                "", "The heatwave has broken", "The cold snap has thawed",
                "The drought has broken", "The storm has passed",
            };
            if (ev.challenge.challenge_type < CHALLENGE_COUNT)
                banner(END_MSGS[ev.challenge.challenge_type]);
            break;
        }

        case EVT_CROP_SOWN: {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s sowed a crop in the garden",
                     ev.crop.who);
            banner(msg);
            break;
        }

        case EVT_CRAFTED: {
            static const char* const KINDS[] = {
                "a sculpture", "a cairn", "a painting", "a memorial",
                "a petal hat", "a seed pendant", "a grass band",
            };
            uint8_t k = (ev.crafted.kind < 7) ? ev.crafted.kind : 0;
            char msg[64];
            if (k == 3 && ev.crafted.honoree[0])
                snprintf(msg, sizeof(msg), "%s carved a memorial for %s",
                         ev.crafted.who, ev.crafted.honoree);
            else if (k >= 4 && ev.crafted.honoree[0])
                snprintf(msg, sizeof(msg), "%s made %s for %s",
                         ev.crafted.who, KINDS[k], ev.crafted.honoree);
            else
                snprintf(msg, sizeof(msg), "%s finished %s",
                         ev.crafted.who, KINDS[k]);
            banner(msg);
            break;
        }

        case EVT_ART_WEATHERED: {
            static const char* const KINDS[] =
                { "sculpture", "cairn", "painting", "memorial" };
            char msg[64];
            snprintf(msg, sizeof(msg), "%s's old %s crumbled away",
                     ev.crafted.who, KINDS[ev.crafted.kind & 3]);
            banner(msg);
            break;
        }

        default:
            break;
        }
    }
}

void Renderer::_draw_scent_marks(const Chamber& ch) {
    for (int i = 0; i < Cfg::MAX_SCENT_MARKS; i++) {
        const Chamber::ScentMark& m = ch.scent_marks[i];
        if (m.ttl == 0) continue;
        int px = m.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int py = m.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        // Fade: bright warm fleck early, dim late
        uint16_t col = (m.ttl > Cfg::SCENT_MARK_TTL / 2)
                     ? _rgb565(214, 188, 120)
                     : _rgb565(150, 130, 90);
        _gfx->fillRect(px - 1, py - 1, 2, 2, col);
        _mark_dirty(px - 2, py - 2, 5, 5);
    }
}

void Renderer::_draw_fireflies(const Chamber& ch, float lerp_t) {
    if (g_tod.night_factor < Cfg::FIREFLY_NIGHT_FACTOR_MIN) return;
    float t = (lerp_t < 0.0f) ? 0.0f : ((lerp_t > 1.0f) ? 1.0f : lerp_t);

    for (int i = 0; i < Cfg::MAX_FIREFLIES; i++) {
        const Firefly& f = ch.fireflies[i];
        if (!f.active) continue;

        float fx = f.prev_x + (f.x - f.prev_x) * t;
        float fy = f.prev_y + (f.y - f.prev_y) * t;
        int px = static_cast<int>(fx * Cfg::CELL_SIZE);
        int py = static_cast<int>(fy * Cfg::CELL_SIZE);

        // Blink: soft pulse, dark half of the cycle = invisible
        float pulse = sinf(f.glow_phase * 0.02454f);  // phase 0-255 ≈ one cycle
        if (pulse <= 0.15f) continue;

        // Fixed warm lantern amber — deliberately NOT from the chamber
        // palette (night-shifted toward blue), and orange-leaning so it
        // pops against the blue night hue
        uint16_t core = _rgb565(255, 225, 120);
        uint16_t halo = _rgb565(230, 140, 30);
        _gfx->drawPixel(px, py, core);
        if (pulse > 0.6f) {
            _gfx->drawPixel(px - 1, py, halo);
            _gfx->drawPixel(px + 1, py, halo);
            _gfx->drawPixel(px, py - 1, halo);
            _gfx->drawPixel(px, py + 1, halo);
        }
        _mark_dirty(px - 2, py - 2, 5, 5);
    }
}

void Renderer::_draw_critters(const Chamber& ch, float lerp_t) {
    float t = (lerp_t < 0.0f) ? 0.0f : ((lerp_t > 1.0f) ? 1.0f : lerp_t);

    for (int i = 0; i < Cfg::MAX_CRITTERS; i++) {
        const Critter& cr = ch.critters[i];
        if (!cr.active) continue;

        float cx = cr.prev_x + (cr.x - cr.prev_x) * t;
        float cy = cr.prev_y + (cr.y - cr.prev_y) * t;
        int px = static_cast<int>(cx * Cfg::CELL_SIZE);
        int py = static_cast<int>(cy * Cfg::CELL_SIZE);

        switch (cr.kind) {
        case CRITTER_BUTTERFLY: {
            // Two wings that flap (open/closed) — a little dab of colour
            bool open = (cr.anim_phase & 0x04);
            uint16_t wing = _rgb565(235, 140, 200);
            uint16_t body = _rgb565(60, 40, 40);
            int spread = open ? 3 : 1;
            _gfx->fillRect(px - spread, py - 1, spread, 3, wing);
            _gfx->fillRect(px + 1,      py - 1, spread, 3, wing);
            _gfx->drawPixel(px, py, body);
            _mark_dirty(px - 4, py - 3, 9, 7);
            break;
        }
        case CRITTER_WORM: {
            // Short wriggling segment
            uint16_t c = _rgb565(210, 120, 120);
            int wig = ((cr.anim_phase >> 1) & 1) ? 1 : 0;
            _gfx->fillRect(px - 2, py + wig, 5, 2, c);
            _mark_dirty(px - 3, py - 1, 8, 5);
            break;
        }
        case CRITTER_MOTH: {
            // Dusty grey night flier — slower flap than the butterfly
            bool open = (cr.anim_phase & 0x08);
            uint16_t wing = _rgb565(190, 185, 168);
            uint16_t body = _rgb565(90, 82, 70);
            int spread = open ? 3 : 1;
            _gfx->fillRect(px - spread, py - 1, spread, 3, wing);
            _gfx->fillRect(px + 1,      py - 1, spread, 3, wing);
            _gfx->drawPixel(px, py, body);
            _mark_dirty(px - 4, py - 3, 9, 7);
            break;
        }
        case CRITTER_SNAIL: {
            // Slow blob with a lighter shell whorl
            uint16_t bodyc = _rgb565(150, 122, 92);
            uint16_t shell = _rgb565(205, 172, 124);
            uint16_t whorl = _rgb565(120, 96, 66);
            _gfx->fillRect(px - 2, py, 5, 2, bodyc);
            _gfx->fillRect(px, py - 2, 3, 3, shell);
            _gfx->drawPixel(px + 1, py - 1, whorl);
            _mark_dirty(px - 3, py - 3, 8, 7);
            break;
        }
        case CRITTER_LADYBIRD: {
            // Little red dome, black spots
            uint16_t red   = _rgb565(205, 45, 40);
            uint16_t black = _rgb565(25, 20, 20);
            _gfx->fillRect(px - 1, py - 1, 3, 3, red);
            _gfx->drawPixel(px - 1, py - 1, black);
            _gfx->drawPixel(px + 1, py, black);
            _gfx->drawPixel(px, py + 2, black);  // head
            _mark_dirty(px - 2, py - 2, 6, 6);
            break;
        }
        case CRITTER_DRAGONFLY: {
            // Long teal dart with shimmering cross-wings
            bool open = (cr.anim_phase & 0x02);
            uint16_t bodyc = _rgb565(60, 185, 190);
            uint16_t wing  = _rgb565(180, 220, 235);
            _gfx->drawFastVLine(px, py - 2, 5, bodyc);
            if (open) {
                _gfx->drawFastHLine(px - 2, py - 1, 5, wing);
                _gfx->drawFastHLine(px - 2, py,     5, wing);
            }
            _mark_dirty(px - 3, py - 3, 7, 8);
            break;
        }
        default: {  // CRITTER_BEETLE — a dark trundling oval with a shell line
            uint16_t shell = _rgb565(70, 55, 40);
            uint16_t back  = _rgb565(110, 90, 60);
            _gfx->fillRect(px - 2, py - 1, 5, 3, shell);
            _gfx->drawPixel(px, py - 1, back);
            _mark_dirty(px - 3, py - 2, 8, 6);
            break;
        }
        }
    }
}

// The maker's palette — same derivation as the body tint, so a work is
// recognisably "a Foxglove piece" from across the room.
void Renderer::_maker_colors(uint8_t tint_seed, uint16_t* main_out, uint16_t* dark_out) {
    uint32_t hs = (uint32_t)tint_seed * 2654435761u;
    float ur = (hs & 0xFFFF) / 65535.0f;
    float uh = ((hs >> 16) & 0xFFFF) / 65535.0f;
    float rare = ur * ur; rare = rare * rare * rare;
    float spread = 85.0f + 155.0f * rare;
    float hue = 28.0f + (uh * 2.0f - 1.0f) * spread;
    while (hue < 0.0f)    hue += 360.0f;
    while (hue >= 360.0f) hue -= 360.0f;
    int r, g, b;
    _hue_vivid(hue, r, g, b);
    float dim = 1.0f - 0.45f * _nf;   // night softens the gallery
    *main_out = _rgb565((uint8_t)(r * dim), (uint8_t)(g * dim), (uint8_t)(b * dim));
    *dark_out = _rgb565((uint8_t)(r * dim * 0.55f), (uint8_t)(g * dim * 0.55f),
                        (uint8_t)(b * dim * 0.55f));
}

// Artifacts — placeholder sprites (v183). Neutral-ramp works go through
// the same luma remap as conker bodies, so one sprite renders in every
// maker's colours. Memorials keep their fixed stone palette (tint 0).
void Renderer::_draw_artworks(const Chamber& ch) {
    for (int i = 0; i < Cfg::MAX_ARTWORKS; i++) {
        const Artwork& a = ch.artworks[i];
        if (!a.active) continue;
        int px = a.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int py = a.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;

        const uint16_t* spr;
        int sw, sh;
        uint8_t tint = a.maker_tint;
        // Sized against the conkers, not the grid: bodies render ~24px, so
        // works at the old 1.4x (~14px) read as trinkets — an arch "to pass
        // under" drew smaller than the one passing under it.
        float scale = 2.2f;

        switch (a.kind) {
        case ART_SCULPTURE:
            switch (a.motif % 3) {
            case 0:  spr = ART_ORB;   sw = ART_ORB_W;   sh = ART_ORB_H;   break;
            case 1:  spr = ART_SPIRE; sw = ART_SPIRE_W; sh = ART_SPIRE_H; break;
            default: spr = ART_ARCH;  sw = ART_ARCH_W;  sh = ART_ARCH_H;  break;
            }
            break;
        case ART_CAIRN:
            spr = ART_CAIRN_SPR; sw = ART_CAIRN_SPR_W; sh = ART_CAIRN_SPR_H;
            scale = 2.0f;
            break;
        case ART_PAINTING:
            if (a.motif % 2) { spr = ART_PAINT_B; sw = ART_PAINT_B_W; sh = ART_PAINT_B_H; }
            else             { spr = ART_PAINT_A; sw = ART_PAINT_A_W; sh = ART_PAINT_A_H; }
            scale = 1.7f;   // paintings lie flat, subtler
            break;
        default:  // ART_MEMORIAL — fixed stone palette, never re-hued
            spr = ART_MEMORIAL_SPR; sw = ART_MEMORIAL_SPR_W; sh = ART_MEMORIAL_SPR_H;
            tint = 0;
            scale = 2.0f;   // a stone that stands
            break;
        }

        _draw_sprite_scaled_tinted(px, py - 3, spr, sw, sh, scale, false, tint);
        int dw = (int)(sw * scale), dh = (int)(sh * scale);
        _mark_dirty(px - dw / 2 - 1, py - 3 - dh / 2 - 1, dw + 3, dh + 3);
    }
}

// Garden crops — procedural placeholders until the artist's plant stages
// land. Bare plots read as a tilled dark patch; stages grow visibly.
void Renderer::_draw_plants(const Chamber& ch) {
    if (!ch.is_garden) return;
    for (int i = 0; i < Cfg::GARDEN_PLOTS; i++) {
        const Plant& p = ch.plants[i];
        int px = p.x * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int py = p.y * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;

        // Tilled soil bed (always visible so plots read as *places*)
        uint16_t soil = _rgb565(_lerp8(105, 60, _nf), _lerp8(82, 48, _nf), _lerp8(58, 38, _nf));
        _gfx->fillRoundRect(px - 6, py - 3, 12, 7, 2, soil);

        switch (p.stage) {
        case PLOT_SPROUT:
            _draw_sprite_scaled_tinted(px, py - 2, PLANT_SPROUT,
                                       PLANT_SPROUT_W, PLANT_SPROUT_H, 1.2f, false, 0);
            break;
        case PLOT_GROWING:
            _draw_sprite_scaled_tinted(px, py - 4, PLANT_GROWING,
                                       PLANT_GROWING_W, PLANT_GROWING_H, 1.3f, false, 0);
            break;
        case PLOT_MATURE:
            _draw_sprite_scaled_tinted(px, py - 5, PLANT_MATURE,
                                       PLANT_MATURE_W, PLANT_MATURE_H, 1.4f, false, 0);
            break;
        default:
            break;   // bare soil
        }
        _mark_dirty(px - 9, py - 14, 19, 22);
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

    case ANIM_HEARTS: {
        // Two tiny hearts drift up and apart, fading as they rise
        uint8_t fade = static_cast<uint8_t>(255 * (1.0f - t * 0.7f));
        uint16_t col = _rgb565(fade, fade * 120 / 255, fade * 145 / 255);
        _gfx->setTextSize(1);
        _gfx->setTextColor(col);
        for (int h = 0; h < 2; h++) {
            int hx = a.px + (h == 0 ? -7 : 3) - static_cast<int>(t * 2) * (h == 0 ? 1 : -1);
            int hy = a.py - 4 - static_cast<int>(t * 12) - h * 3;
            _gfx->setCursor(hx, hy);
            _gfx->print('\x03');
            _mark_dirty(hx, hy, 6, 8);
        }
        break;
    }

    case ANIM_TRAIT_SPARKLE: {
        // Gold burst — like a hatch pop but grander, white-hot at the end
        int arm = 3 + static_cast<int>(10 * t);
        uint16_t col = (t < 0.5f) ? _pal_glow_amber
                                  : _rgb565(255, 245, 215);
        _gfx->drawFastHLine(a.px - arm, a.py, arm * 2 + 1, col);
        _gfx->drawFastVLine(a.px, a.py - arm, arm * 2 + 1, col);
        int d = arm * 7 / 10;
        _gfx->drawPixel(a.px - d, a.py - d, col);
        _gfx->drawPixel(a.px + d, a.py - d, col);
        _gfx->drawPixel(a.px - d, a.py + d, col);
        _gfx->drawPixel(a.px + d, a.py + d, col);
        _mark_dirty(a.px - arm - 1, a.py - arm - 1, arm * 2 + 3, arm * 2 + 3);
        break;
    }
    }
}

// ================================================================
//  Story-beat banner — one-line narration under the HUD
// ================================================================

// Display labels for TraitBit masks (order matches world_condition.h)
const char* Renderer::_trait_label(uint32_t bit) {
    switch (bit) {
        case 1u << 0: return "Pioneer";
        case 1u << 1: return "Elder";
        case 1u << 2: return "Bonded";
        case 1u << 3: return "Heatwave Survivor";
        case 1u << 4: return "Cold Snap Survivor";
        case 1u << 5: return "Drought Survivor";
        case 1u << 6: return "Storm Survivor";
        case 1u << 7: return "Bug Hunter";
        default:      return "?";
    }
}

void Renderer::banner(const char* text) {
    if (_banner_q_count >= MAX_BANNERS) return;  // advisory — drop when full
    strlcpy(_banner_q[_banner_q_count++], text, sizeof(_banner_q[0]));
}

void Renderer::_tick_banner() {
    static constexpr int BANNER_Y = 32;   // just under the 28px HUD strip
    static constexpr int BANNER_H = 14;

    if (_banner_q_count == 0) return;
    unsigned long now = millis();
    if (_banner_front_ms == 0) _banner_front_ms = now;

    if (now - _banner_front_ms >= BANNER_SHOW_MS) {
        // Dismiss front, repaint the vacated strip, promote the next
        for (int i = 1; i < _banner_q_count; i++)
            strlcpy(_banner_q[i - 1], _banner_q[i], sizeof(_banner_q[0]));
        _banner_q_count--;
        _banner_front_ms = (_banner_q_count > 0) ? now : 0;
        _mark_dirty(_banner_rect_x, BANNER_Y, _banner_rect_w, BANNER_H);
        if (_banner_q_count == 0) return;
    }

    const char* text = _banner_q[0];
    int tw = 6 * static_cast<int>(strlen(text));
    int bw = tw + 10;
    int bx = (SCREEN_W - bw) / 2;

    _gfx->fillRoundRect(bx, BANNER_Y, bw, BANNER_H, 3, _rgb565(38, 28, 20));
    _gfx->drawRoundRect(bx, BANNER_Y, bw, BANNER_H, 3, _pal_glow_amber);
    _gfx->setTextSize(1);
    _gfx->setTextColor(_rgb565(255, 240, 210));
    _gfx->setCursor(bx + 5, BANNER_Y + 3);
    _gfx->print(text);
    _mark_dirty(bx, BANNER_Y, bw, BANNER_H);
    _banner_rect_x = bx;
    _banner_rect_w = bw;
}

// ================================================================
//  Tunnel entrances — sprite-based burrow openings at connected faces
// ================================================================

#include "sprites.h"

void Renderer::_draw_tunnel_entrances(const Chamber& ch) {
    for (int f = 0; f < FACE_COUNT; f++) {
        if (ch.entries[f] < 0) continue;

        int cx = Cfg::ENTRY_X[f] * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;
        int cy = Cfg::ENTRY_Y[f] * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 2;

        // TUNNEL_E is 20w × 56h, designed for the east (right) edge.
        // Anchor: right edge of sprite flush with screen edge, centered vertically.
        int sx, sy;
        const uint16_t* data = TUNNEL_E;
        int sw = TUNNEL_E_W, sh = TUNNEL_E_H;

        float scale = 1.3f;
        int scaled_w = (int)(sw * scale);
        int scaled_h = (int)(sh * scale);

        switch (f) {
        case FACE_E:
            sx = SCREEN_W - scaled_w + 10;
            sy = cy - scaled_h / 2;
            _draw_sprite_scaled(sx + scaled_w/2, sy + scaled_h/2, data, sw, sh, scale, false);
            break;
        case FACE_W:
            sx = -10;
            sy = cy - scaled_h / 2;
            _draw_sprite_scaled(sx + scaled_w/2, sy + scaled_h/2, data, sw, sh, scale, true);
            break;
        case FACE_N:
            // Rotate 90° CCW + scale
            {
                int dst_w = (int)(sh * scale), dst_h = (int)(sw * scale);
                sx = cx - dst_w / 2;
                sy = -10;
                for (int dy = 0; dy < dst_h; dy++) {
                    int src_col = (int)((sw - 1) - dy / scale);
                    for (int dx = 0; dx < dst_w; dx++) {
                        int src_row = (int)(dx / scale);
                        if (src_row >= sh || src_col < 0) continue;
                        uint16_t col = pgm_read_word(&data[src_row * sw + src_col]);
                        if (col != SPRITE_TRANSPARENT)
                            _gfx->drawPixel(sx + dx, sy + dy, col);
                    }
                }
                _mark_dirty(sx, sy, dst_w, dst_h);
            }
            continue;
        case FACE_S:
            // Rotate 90° CW + scale
            {
                int dst_w = (int)(sh * scale), dst_h = (int)(sw * scale);
                sx = cx - dst_w / 2;
                sy = SCREEN_H - dst_h + 10;
                for (int dy = 0; dy < dst_h; dy++) {
                    int src_col = (int)(dy / scale);
                    for (int dx = 0; dx < dst_w; dx++) {
                        int src_row = (int)((sh - 1) - dx / scale);
                        if (src_row < 0 || src_col >= sw) continue;
                        uint16_t col = pgm_read_word(&data[src_row * sw + src_col]);
                        if (col != SPRITE_TRANSPARENT)
                            _gfx->drawPixel(sx + dx, sy + dy, col);
                    }
                }
                _mark_dirty(sx, sy, dst_w, dst_h);
            }
            continue;
        default: continue;
        }

        _mark_dirty(sx, sy, sw, sh);
    }
}

// ================================================================
//  Debug: pheromone heatmap overlay
// ================================================================

void Renderer::_draw_phero_overlay(const Chamber& ch) {
    // Draw semi-transparent colored cells:
    //   Home pheromone = blue channel
    //   Food pheromone = green channel
    // Intensity mapped to alpha (skip cells with no pheromone)
    for (int cy = 0; cy < Cfg::GRID_HEIGHT; cy++) {
        for (int cx = 0; cx < Cfg::GRID_WIDTH; cx++) {
            float h = ch.pheromones.raw_home(cx, cy);
            float f = ch.pheromones.raw_food(cx, cy);
            if (h < 0.01f && f < 0.01f) continue;

            // Normalize to 0–1 range (clamp at 20 — typical max from boundary sync)
            float hn = (h > 20.0f) ? 1.0f : h / 20.0f;
            float fn = (f > 20.0f) ? 1.0f : f / 20.0f;

            // Map to RGB: home=blue, food=green
            int r = 0;
            int g = static_cast<int>(fn * 200);
            int b = static_cast<int>(hn * 200);

            // Alpha blend at ~40% opacity over each pixel in cell
            // For speed, just draw a single smaller rect at ~50% cell size
            int px = cx * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 4;
            int py = cy * Cfg::CELL_SIZE + Cfg::CELL_SIZE / 4;
            int pw = Cfg::CELL_SIZE / 2;
            int ph = Cfg::CELL_SIZE / 2;

            uint16_t col = _rgb565(r, g, b);
            _gfx->fillRect(px, py, pw, ph, col);
        }
    }
}

// ================================================================
//  Weather particle effects — persistent particles
// ================================================================

static constexpr int WX_MAX_DROPS = 50;
static struct { float x, y, vy; } _wx_drops[WX_MAX_DROPS];
static int _wx_drop_count = 0;
static constexpr int WX_MAX_SNOW = 25;
static struct { float x, y, phase; } _wx_snow[WX_MAX_SNOW];
static int _wx_snow_count = 0;
static constexpr int WX_FOG_WISPS = 8;
static float _wx_fog_y[WX_FOG_WISPS];
static float _wx_fog_x[WX_FOG_WISPS];
static float _wx_fog_hw[WX_FOG_WISPS];
// Pre-computed pixel offsets per wisp (stable, no flicker)
static constexpr int WX_FOG_PTS = 40;
static int8_t _wx_fog_dx[WX_FOG_WISPS][WX_FOG_PTS];
static int8_t _wx_fog_dy[WX_FOG_WISPS][WX_FOG_PTS];
static WeatherCondition _wx_prev = WX_CLEAR;
static int _wx_lightning_cooldown = 0;
static int _wx_lightning_flash = 0;   // frames remaining for current flash
static int _wx_flash_x = 0;          // lightning bolt x position

static void _wx_init_drops(int count, float vy_min, float vy_max) {
    _wx_drop_count = count;
    for (int i = 0; i < count; i++) {
        _wx_drops[i].x  = g_rng.rand_float() * SCREEN_W;
        _wx_drops[i].y  = g_rng.rand_float() * SCREEN_H;
        _wx_drops[i].vy = vy_min + g_rng.rand_float() * (vy_max - vy_min);
    }
}

static void _wx_init_snow(int count) {
    _wx_snow_count = count;
    for (int i = 0; i < count; i++) {
        _wx_snow[i].x     = g_rng.rand_float() * SCREEN_W;
        _wx_snow[i].y     = g_rng.rand_float() * SCREEN_H;
        _wx_snow[i].phase = g_rng.rand_float() * 6.28f;
    }
}

static void _wx_init_fog() {
    for (int i = 0; i < WX_FOG_WISPS; i++) {
        _wx_fog_y[i]  = g_rng.rand_float() * SCREEN_H;
        _wx_fog_x[i]  = g_rng.rand_float() * SCREEN_W * 1.5f - SCREEN_W * 0.25f;
        _wx_fog_hw[i] = 90.0f + g_rng.rand_float() * 80.0f;
        // Pre-compute scattered pixel offsets (Gaussian-ish via sum of randoms)
        int hw = (int)_wx_fog_hw[i];
        int hh = 8 + (i & 3) * 3;
        for (int p = 0; p < WX_FOG_PTS; p++) {
            // Sum two randoms for softer distribution (peaks at center)
            _wx_fog_dx[i][p] = (int8_t)((g_rng.rand_int(-hw, hw) + g_rng.rand_int(-hw, hw)) / 2);
            _wx_fog_dy[i][p] = (int8_t)((g_rng.rand_int(-hh, hh) + g_rng.rand_int(-hh, hh)) / 2);
        }
    }
}

void Renderer::_draw_weather() {
    if (!g_weather.valid) return;

    // Re-init particles when condition changes
    WeatherCondition wx = g_weather.condition;
    if (wx != _wx_prev) {
        _wx_prev = wx;
        _wx_drop_count = 0;
        _wx_snow_count = 0;
        _wx_lightning_cooldown = 0;
        _wx_lightning_flash = 0;
        // All rain falls fast — slow 2px drizzle drops read as snowflakes
        // (Amber). Volume is the difference between conditions, not speed.
        switch (wx) {
        case WX_DRIZZLE:    _wx_init_drops(12, 4.5f, 6.5f); break;
        case WX_RAIN:       _wx_init_drops(35, 6.0f, 8.5f); break;
        case WX_HEAVY_RAIN: _wx_init_drops(50, 8.0f, 11.0f); break;
        case WX_THUNDERSTORM: _wx_init_drops(40, 6.5f, 9.0f); break;
        case WX_SNOW:       _wx_init_snow(20); break;
        case WX_FOG:        _wx_init_fog(); break;
        default: break;
        }
    }

    // --- Rain / Drizzle / Heavy Rain ---
    if (wx == WX_DRIZZLE || wx == WX_RAIN || wx == WX_HEAVY_RAIN ||
        (wx == WX_THUNDERSTORM && _wx_drop_count > 0)) {
        uint16_t col;
        int len;
        if (wx == WX_DRIZZLE) {
            col = _rgb565(150, 165, 190); len = 3;
        } else if (wx == WX_RAIN) {
            col = _rgb565(130, 145, 180); len = 4;
        } else {
            col = _rgb565(110, 125, 170); len = 5;
        }
        for (int i = 0; i < _wx_drop_count; i++) {
            auto& d = _wx_drops[i];
            d.y += d.vy;
            d.x += 0.4f;  // slight wind drift
            if (d.y > SCREEN_H + 5) {
                d.y = g_rng.rand_float() * -30.0f;
                d.x = g_rng.rand_float() * SCREEN_W;
            }
            if (d.x > SCREEN_W) d.x -= SCREEN_W;
            int px = (int)d.x, py = (int)d.y;
            if (py >= 0 && py < SCREEN_H - len)
                _gfx->drawFastVLine(px, py, len, col);
        }
    }

    // --- Snow ---
    if (wx == WX_SNOW) {
        uint16_t col = _rgb565(215, 220, 230);
        for (int i = 0; i < _wx_snow_count; i++) {
            auto& s = _wx_snow[i];
            s.y += 1.5f;                             // gentle fall
            s.x += sinf(s.phase) * 0.4f;             // gentle wobble
            s.phase += 0.05f;
            if (s.y > SCREEN_H + 2) {
                s.y = -2.0f;
                s.x = g_rng.rand_float() * SCREEN_W;
            }
            if (s.x < 0) s.x += SCREEN_W;
            if (s.x >= SCREEN_W) s.x -= SCREEN_W;
            int px = (int)s.x, py = (int)s.y;
            if (py >= 0 && py < SCREEN_H - 1)
                _gfx->fillRect(px, py, 2, 2, col);
        }
    }

    // --- Fog ---
    if (wx == WX_FOG) {
        uint16_t col1 = _rgb565(180, 175, 162);
        uint16_t col2 = _rgb565(165, 162, 155);
        for (int i = 0; i < WX_FOG_WISPS; i++) {
            _wx_fog_x[i] += 0.10f + i * 0.015f;  // very slow drift
            if (_wx_fog_x[i] > SCREEN_W + _wx_fog_hw[i])
                _wx_fog_x[i] = -_wx_fog_hw[i] * 2;

            int cx = (int)_wx_fog_x[i];
            int cy = (int)_wx_fog_y[i];
            uint16_t col = (i & 1) ? col1 : col2;
            // Draw pre-computed pattern (stable, no flicker)
            for (int p = 0; p < WX_FOG_PTS; p++) {
                int px = cx + _wx_fog_dx[i][p];
                int py = cy + _wx_fog_dy[i][p];
                if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                    _gfx->drawPixel(px, py, col);
            }
        }
    }

    // --- Thunderstorm lightning ---
    if (wx == WX_THUNDERSTORM) {
        if (_wx_lightning_flash > 0) {
            // Active flash — draw a jagged bolt + ambient glow
            _wx_lightning_flash--;
            uint16_t bolt_col = _rgb565(240, 240, 255);
            uint16_t glow_col = _rgb565(200, 200, 230);
            // Zigzag bolt
            int bx = _wx_flash_x;
            for (int sy = 0; sy < SCREEN_H; sy += 12) {
                int ey = sy + 10;
                if (ey > SCREEN_H) ey = SCREEN_H;
                int ex = bx + g_rng.rand_int(-8, 8);
                _gfx->drawLine(bx, sy, ex, ey, bolt_col);
                _gfx->drawLine(bx + 1, sy, ex + 1, ey, glow_col);
                bx = ex;
            }
        } else {
            _wx_lightning_cooldown--;
            if (_wx_lightning_cooldown <= 0 && g_rng.rand_float() < 0.008f) {
                _wx_lightning_flash = 3;   // flash for 3 frames (~100ms)
                _wx_flash_x = g_rng.rand_int(40, SCREEN_W - 40);
                _wx_lightning_cooldown = 90;  // minimum 3 seconds between bolts
            }
        }
    }
}
