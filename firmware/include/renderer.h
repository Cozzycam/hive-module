/* Chamber renderer — 1:1 full-chamber rendering.
 * Grid is 30x20 at CELL_SIZE=16 = 480x320 = full screen.
 * Uses dirty-rect tracking to minimize PSRAM writes.
 * Includes lightweight animation system driven by sim events.
 *
 * Floor rendering: warm textured chamber with radial gradient,
 * grain specks, and edge-decor objects. Three palette states
 * (day/dusk/night) driven by g_tod.night_factor.
 */
#pragma once

#include <Arduino_GFX_Library.h>
#include "Arduino_TFT.h"
#include "chamber.h"
#include "events.h"

// ---- Floor tint (user-set ground colour) ----
// Free functions: state lives beside the palette in renderer.cpp.
// (0,0,0) = no tint. persist=true writes NVS ("hive"/"tint").
void     renderer_set_floor_tint(uint8_t r, uint8_t g, uint8_t b, bool persist);
void     renderer_load_floor_tint();          // call once at boot
uint32_t renderer_get_floor_tint();           // 0xRRGGBB, 0 = none

// Async-flush drain: wait until no framebuffer push is in flight and take
// permanent ownership of the canvas. Modal paths that paint + flush outside
// the frame loop (OTA splash) MUST call this first; they never hand the
// canvas back (every OTA window exits via reboot).
void     renderer_flush_drain();

// Compile-time flags
#ifndef BOOT_SPLASH_ENABLED
#define BOOT_SPLASH_ENABLED 1
#endif

#ifndef RENDERER_FORCE_FULL_REDRAW
#define RENDERER_FORCE_FULL_REDRAW 0
#endif

// ---- Animation types ----
enum AnimType : uint8_t {
    ANIM_TAP_RING = 0,     // expanding circle at tap position
    ANIM_FOOD_DELIVER,     // bright dot floats up at delivery
    ANIM_EGG_LAID,         // brief egg-colored glow
    ANIM_HATCH,            // expanding sparkle pop
    ANIM_DEATH_WORKER,     // brief X mark
    ANIM_DEATH_YOUNG,      // dim fade dot
    ANIM_HEARTS,           // hearts drift up — a bond formed / best friends
    ANIM_TRAIT_SPARKLE,    // gold burst — a trait/title earned
};

struct Anim {
    AnimType type;
    int16_t  px, py;     // screen pixel center
    uint8_t  age;        // frames elapsed
    uint8_t  duration;   // total frames
    bool     active;
};

// ---- Chamber floor palette ----
struct ChamberPalette {
    uint8_t outer_r, outer_g, outer_b;   // bgOuter (frame)
    uint8_t floor1_r, floor1_g, floor1_b; // inner radial
    uint8_t floor2_r, floor2_g, floor2_b; // outer radial
    uint8_t grain_r, grain_g, grain_b;    // grain speck
    uint8_t amb_r, amb_g, amb_b, amb_a;   // ambient wash
};

// ---- Grain speck (precomputed) ----
struct GrainSpeck {
    int16_t x, y;
    uint8_t radius_x10;  // radius * 10 (6–18 = 0.6–1.8px)
};

// ---- Sprite z-ordering ----
enum SpriteKind : uint8_t {
    SK_FOOD_PILE = 0,
    SK_EGG,
    SK_LARVA,      // legacy alias = SK_SEED
    SK_PUPA,       // legacy alias = SK_HUSK
    SK_WORKER,
    SK_QUEEN,
    SK_HUSK,
};
constexpr SpriteKind SK_SEED = SK_LARVA;

struct SpriteDraw {
    float    sort_y;      // Y for depth sorting (may include bias)
    int16_t  render_x;    // screen pixel center
    int16_t  render_y;
    uint16_t size_px;     // rendered size after scaling (for tiebreaker)
    SpriteKind kind;
    uint8_t  flags;       // bit 0 = flip_h
    int16_t  entity_idx;  // index into sim array
    uint8_t  tint_seed;   // per-entity colour variation seed (workers only)
};

class Renderer {
public:
    // canvas2: optional second framebuffer for double-buffered async flush
    // (pass null to run single-buffered).
    void init(Arduino_Canvas* canvas, Arduino_Canvas* canvas2,
              Arduino_TFT* output);
    void draw(const Chamber& ch, float lerp_t);
    void flush();
    // The canvas the CURRENT frame draws into. Overlays painted between
    // draw() and flush() (selection ring, HUD, debug) must use this, not a
    // cached pointer — it alternates every frame when double-buffered.
    Arduino_Canvas* canvas() const { return _gfx; }
    void force_full_redraw() { _needs_full_redraw = true; }
    void mark_dirty_external(int x, int y, int w, int h) { _mark_dirty(x, y, w, h); }
    bool debug_phero = false;  // pheromone overlay (toggle via serial 'p')
    bool force_full_flush = RENDERER_FORCE_FULL_REDRAW;  // debug: disable dirty-rect flush

    // Feed drained events to create animations
    void receive_events(const Event* events, int count, const Chamber& ch);

    // Story-beat banner — one-line narration under the HUD ("X & Y are
    // best friends"). Queued; each shows ~3.5s. Advisory: drops when full.
    void banner(const char* text);

    // Milestone decor — colony achievements grow permanent objects into
    // the floor. Bits: 0 = mossy stone (25 workers born), 1 = cairn
    // (survived a challenge), 2 = wildflower (first best friends),
    // 3 = golden seed (Bug Hunter crowned). Change invalidates floor cache.
    void set_milestone_decor(uint8_t bits);

    // Boot splash
    void start_boot_splash();
    bool is_splash_active() const { return _boot_splash_active; }

private:
    Arduino_Canvas* _gfx = nullptr;
    Arduino_TFT* _output = nullptr;  // Direct access to display for windowed flush
    bool _needs_full_redraw = true;
    uint32_t _frame = 0;   // monotonic frame counter for idle animations

    struct DirtyRect { int16_t x, y, w, h; };
    static constexpr int MAX_DIRTY = 256;
    DirtyRect _dirty[MAX_DIRTY];
    int _dirty_count = 0;

    // Flush bounding box — union of previous + current frame dirty rects
    struct FlushBounds {
        int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool any = false;
        bool full = false;  // force full-frame flush
    };
    FlushBounds _flush_bounds;
    FlushBounds _prev_flush_bounds;  // Previous frame's bounds (need re-flush after floor restore)
    void _union_dirty_into_bounds(FlushBounds& bounds);
    void _union_bounds(FlushBounds& dst, const FlushBounds& src);

    // Animation pool
    static constexpr int MAX_ANIMS = 32;
    Anim _anims[MAX_ANIMS];
    int  _anim_count = 0;

    // Floor grain specks (precomputed at init)
    static constexpr int NUM_GRAIN = 30;
    GrainSpeck _grain[NUM_GRAIN];

    // Boot splash state
    bool _boot_splash_active = false;
    unsigned long _boot_splash_start_ms = 0;

    // Z-sorted sprite draw lists (static buffers, reused each frame)
    static constexpr int MAX_FLOOR_SPRITES = Cfg::MAX_BROOD + Cfg::MAX_FOOD_PILES;
    static constexpr int MAX_AGENT_SPRITES = Cfg::MAX_CONKERS + 1; // +1 queen
    SpriteDraw _floor_sprites[MAX_FLOOR_SPRITES];
    int _floor_sprite_count = 0;
    SpriteDraw _agent_sprites[MAX_AGENT_SPRITES];
    int _agent_sprite_count = 0;

    void _mark_dirty(int sx, int sy, int sw, int sh);
    void _clear_dirty();

    // Floor rendering — cached path
    void _render_floor_to_cache();
    void _blit_floor_cache_full();

    // Floor rendering — uncached fallback
    void _draw_floor_uncached();
    void _draw_edge_decor_direct();
    uint8_t _milestone_decor = 0;
    void _draw_milestone_decor();

    // Boot splash
    bool _tick_boot_splash(const Chamber& ch);
    void _dim_framebuffer(float brightness);

    // Z-sorted entity rendering
    void _build_floor_sprites(const Chamber& ch);
    void _build_agent_sprites(const Chamber& ch, float lerp_t);
    void _draw_sorted_sprites(SpriteDraw* list, int count, const Chamber& ch);
    void _draw_one_sprite(const SpriteDraw& sd, const Chamber& ch);

    void _draw_sprite(int cx, int cy, const uint16_t* data,
                      int sw, int sh, bool flip_h = false);
    void _draw_sprite_scaled(int cx, int cy, const uint16_t* data,
                             int sw, int sh, float scale,
                             bool flip_h = false);
    void _draw_sprite_scaled_tinted(int cx, int cy, const uint16_t* data,
                                    int sw, int sh, float scale,
                                    bool flip_h, uint8_t tint_seed,
                                    float age_grey = 0.0f);

    // Kept for boot splash compatibility
    void _draw_queen(const Chamber& ch);

    // Animations
    void _spawn_anim(AnimType type, int px, int py, uint8_t duration);
    void _draw_anims();
    void _draw_one_anim(const Anim& a);

    // Story-beat banner queue
    static constexpr int MAX_BANNERS = 4;
    static constexpr uint32_t BANNER_SHOW_MS = 3500;
    char _banner_q[MAX_BANNERS][64];
    int  _banner_q_count = 0;
    unsigned long _banner_front_ms = 0;   // when the front banner went up (0 = idle)
    int16_t _banner_rect_x = 0, _banner_rect_w = 0;  // last drawn box (for dismissal repaint)
    void _tick_banner();
    static const char* _trait_label(uint32_t bit);

    // Tunnel entrances at connected faces
    void _draw_tunnel_entrances(const Chamber& ch);

    // Debug: pheromone overlay
    void _draw_phero_overlay(const Chamber& ch);

    // Weather particle effects
    void _draw_weather();

    // Night ambience: blinking firefly glows (sim-owned, lerped)
    void _draw_fireflies(const Chamber& ch, float lerp_t);

    // Visiting critters: beetle/butterfly/worm wandering the chamber
    void _draw_critters(const Chamber& ch, float lerp_t);

    // Garden crops (plots + growth stages; garden module only)
    void _draw_plants(const Chamber& ch);

    // Artifacts — conker-made works, rendered in their maker's hue
    void _draw_artworks(const Chamber& ch);
    void _maker_colors(uint8_t tint_seed, uint16_t* main_out, uint16_t* dark_out);

    // Finger scent shimmer — fading echo of a swipe
    void _draw_scent_marks(const Chamber& ch);
};
