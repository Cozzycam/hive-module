/* Chamber renderer — 1:1 full-chamber rendering.
 * Grid is 30x20 at CELL_SIZE=16 = 480x320 = full screen.
 * Uses dirty-rect tracking to minimize PSRAM writes.
 * Includes lightweight animation system driven by sim events.
 *
 * Floor rendering: warm textured chamber with radial gradient,
 * grain specks, and edge-decor objects. Three palette states
 * (day/dusk/night) driven by g_tod.night_factor.
 *
 * Sprout is rendered per-frame as an overlay (not cached) so it
 * can animate: sun-tracking tilt, milestone leaf growth + shimmer.
 */
#pragma once

#include <Arduino_GFX_Library.h>
#include "chamber.h"
#include "events.h"

// Compile-time flags
#ifndef BOOT_SPLASH_ENABLED
#define BOOT_SPLASH_ENABLED 1
#endif

// ---- Animation types ----
enum AnimType : uint8_t {
    ANIM_TAP_RING = 0,     // expanding circle at tap position
    ANIM_FOOD_DELIVER,     // bright dot floats up at delivery
    ANIM_EGG_LAID,         // brief egg-colored glow
    ANIM_HATCH,            // expanding sparkle pop
    ANIM_DEATH_WORKER,     // brief X mark
    ANIM_DEATH_YOUNG,      // dim fade dot
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
    SK_LARVA,
    SK_PUPA,
    SK_WORKER,
    SK_QUEEN,
};

struct SpriteDraw {
    float    sort_y;      // Y for depth sorting (may include bias)
    int16_t  render_x;    // screen pixel center
    int16_t  render_y;
    uint16_t size_px;     // rendered size after scaling (for tiebreaker)
    SpriteKind kind;
    uint8_t  flags;       // bit 0 = flip_h
    int16_t  entity_idx;  // index into sim array
};

class Renderer {
public:
    void init(Arduino_Canvas* canvas);
    void draw(const Chamber& ch, float lerp_t);
    void flush();
    void force_full_redraw() { _needs_full_redraw = true; }

    // Feed drained events to create animations
    void receive_events(const Event* events, int count, const Chamber& ch);

    // Boot splash
    void start_boot_splash();
    bool is_splash_active() const { return _boot_splash_active; }

    // Reset sprout to base state (call on colony reset)
    void reset_sprout();

private:
    Arduino_Canvas* _gfx = nullptr;
    bool _needs_full_redraw = true;
    uint32_t _frame = 0;   // monotonic frame counter for idle animations

    struct DirtyRect { int16_t x, y, w, h; };
    static constexpr int MAX_DIRTY = 256;
    DirtyRect _dirty[MAX_DIRTY];
    int _dirty_count = 0;

    // Animation pool
    static constexpr int MAX_ANIMS = 32;
    Anim _anims[MAX_ANIMS];
    int  _anim_count = 0;

    // Floor grain specks (precomputed at init)
    static constexpr int NUM_GRAIN = 30;
    GrainSpeck _grain[NUM_GRAIN];

    // Sprout state
    int   _sprout_leaf_count = 3;
    uint16_t _last_milestone_born = 0;
    float _leaf_grow_t = -1.0f;     // -1 = idle, 0..1 = new leaf growing
    float _leaf_shimmer_t = -1.0f;  // -1 = idle, 0..1 = gold shimmer

    // Boot splash state
    bool _boot_splash_active = false;
    unsigned long _boot_splash_start_ms = 0;

    // Z-sorted sprite draw lists (static buffers, reused each frame)
    static constexpr int MAX_FLOOR_SPRITES = Cfg::MAX_BROOD + Cfg::MAX_FOOD_PILES;
    static constexpr int MAX_AGENT_SPRITES = Cfg::MAX_LIL_GUYS + 1; // +1 queen
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

    // Sprout (per-frame overlay, not cached)
    void _draw_sprout_overlay();
    void _draw_sprout_direct(int x, int y, float tilt_angle, int leaf_count,
                             float grow_t, float shimmer_t);

    // Milestone check
    void _check_milestone(const Chamber& ch);

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

    // Kept for boot splash compatibility
    void _draw_queen(const Chamber& ch);

    // Animations
    void _spawn_anim(AnimType type, int px, int py, uint8_t duration);
    void _draw_anims();
    void _draw_one_anim(const Anim& a);
};
