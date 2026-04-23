/* Status HUD — horizontal strip overlay at top of 480x320 display.
 *
 * Layout (left to right):
 *   [pop icon] 127 LilGuys · Day 47 · [food icon] 9 days food ... growing [pulse dot]
 *
 * Three palette states (day/dusk/night) derived from g_tod.night_factor + phase.
 * Numbers animate toward target over ~200ms (smooth interpolation).
 */
#include "hud.h"
#include "time_of_day.h"
#include "config.h"
#include <Preferences.h>
#include <cstdio>
#include <cmath>

// ================================================================
//  Layout constants
// ================================================================
static constexpr int HUD_STRIP_H    = 28;
static constexpr int HUD_PAD_X      = 10;
static constexpr int HUD_TEXT_Y     = 9;   // baseline offset within strip (centers 11px text in 28px)
static constexpr int SCREEN_W       = 480;

// ================================================================
//  Palette — three lighting states
// ================================================================
struct HudPalette {
    uint8_t bg_r, bg_g, bg_b, bg_a;   // background RGBA (alpha 0-255)
    uint8_t rim_r, rim_g, rim_b, rim_a;
    uint8_t ink_r, ink_g, ink_b;       // primary text (values)
    uint8_t ink2_r, ink2_g, ink2_b;    // secondary text (labels)
    uint8_t acc_r, acc_g, acc_b;       // accent (icons)
    uint8_t moss_r, moss_g, moss_b;    // phase dot
};

static constexpr HudPalette PAL_DAY = {
    251, 244, 231, 235,    // hudBg rgba(251,244,231,0.92)
    139, 106,  62,  64,    // hudRim rgba(139,106,62,0.25)
     43,  36,  29,         // hudInk #2B241D
    107,  90,  70,         // hudInk2 #6B5A46
    201, 137,  42,         // hudAccent #C9892A
    107, 142,  78,         // hudMoss #6B8E4E
};

static constexpr HudPalette PAL_DUSK = {
    236, 214, 180, 224,    // hudBg rgba(236,214,180,0.88)
     89,  46,  28,  77,    // hudRim rgba(89,46,28,0.30)
     43,  30,  24,         // hudInk #2B1E18
    107,  74,  56,         // hudInk2 #6B4A38
    184,  95,  62,         // hudAccent #B85F3E
    122, 142,  90,         // hudMoss #7A8E5A
};

static constexpr HudPalette PAL_NIGHT = {
     72,  68,  88, 199,    // hudBg rgba(72,68,88,0.78)
    200, 180, 140,  51,    // hudRim rgba(200,180,140,0.20)
    240, 228, 200,         // hudInk #F0E4C8
    184, 169, 138,         // hudInk2 #B8A98A
    233, 200, 121,         // hudAccent #E9C879
    154, 176, 128,         // hudMoss #9AB080
};

// ================================================================
//  Colony phase state machine
// ================================================================
enum ColonyPhase : uint8_t {
    PHASE_FOUNDING = 0,
    PHASE_GROWING  = 1,
    PHASE_MATURE   = 2,
};

static const char* PHASE_LABELS[] = { "founding", "growing", "mature" };

static ColonyPhase _get_phase(int population) {
    if (population < 10)  return PHASE_FOUNDING;
    if (population < 200) return PHASE_GROWING;
    return PHASE_MATURE;
}

// ================================================================
//  Colony age (NVS-persisted founding time)
// ================================================================
static Preferences _prefs;
static uint32_t _colony_founded_unix = 0;
static bool     _founded_stored      = false;
static bool     _founded_reliable    = false;  // stored from NTP/RTC, not simulated clock

static void _load_founded() {
    _prefs.begin("hive", false);
    _colony_founded_unix = _prefs.getULong("founded", 0);
    _founded_reliable    = _prefs.getBool("founded_ok", false);
    _prefs.end();
    _founded_stored = (_colony_founded_unix != 0);
}

static void _store_founded(uint32_t t, bool reliable) {
    _colony_founded_unix = t;
    _founded_stored = true;
    _founded_reliable = reliable;
    _prefs.begin("hive", false);
    _prefs.putULong("founded", t);
    _prefs.putBool("founded_ok", reliable);
    _prefs.end();
}

static uint32_t _colony_age_days() {
    if (_colony_founded_unix == 0) return 0;
    if (g_tod.unix_time <= _colony_founded_unix) return 0;
    return (g_tod.unix_time - _colony_founded_unix) / 86400;
}

// ================================================================
//  Food reserves (real-life days remaining)
// ================================================================
static float _food_days_remaining(const Chamber& ch) {
    float store = ch.colony->food_store;
    if (store < 0.01f) return 0.0f;
    float burn = ch.colony->daily_burn();
    if (burn < 0.001f) return 99.0f;
    float days = store / burn;
    return (days > 99.0f) ? 99.0f : days;
}

// ================================================================
//  Animated values (smooth lerp toward target over ~200ms)
// ================================================================
struct AnimVal {
    float current;
    float target;
    float velocity;  // not used, simple lerp
};

static AnimVal _anim_pop  = { 0, 0, 0 };
static AnimVal _anim_days = { 0, 0, 0 };
static AnimVal _anim_food = { 0, 0, 0 };

static void _update_anim(AnimVal& v, float dt) {
    // Exponential ease toward target (~200ms to converge)
    float rate = 10.0f;  // ~100ms half-life at 30fps
    float diff = v.target - v.current;
    if (fabsf(diff) < 0.5f) {
        v.current = v.target;
    } else {
        v.current += diff * (1.0f - expf(-rate * dt));
    }
}

// ================================================================
//  Palette interpolation helpers
// ================================================================
static inline uint8_t _lerp8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)(a + (b - a) * t);
}

static inline uint16_t _rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Blend src color over dst with alpha (0-255). Returns RGB565.
static inline uint16_t _blend_over(uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
                                   uint16_t dst) {
    if (sa == 255) return _rgb565(sr, sg, sb);
    if (sa == 0)   return dst;

    // Extract dst
    uint8_t dr = ((dst >> 11) & 0x1F) << 3;
    uint8_t dg = ((dst >> 5)  & 0x3F) << 2;
    uint8_t db = (dst         & 0x1F) << 3;

    float a = sa / 255.0f;
    uint8_t r = (uint8_t)(sr * a + dr * (1.0f - a));
    uint8_t g = (uint8_t)(sg * a + dg * (1.0f - a));
    uint8_t b = (uint8_t)(sb * a + db * (1.0f - a));
    return _rgb565(r, g, b);
}

// Get interpolated palette based on night_factor and phase
static void _get_palette(HudPalette& out) {
    float nf = g_tod.night_factor;
    DayPhase phase = g_tod.phase;

    // Day → Dusk: nf 0.0–0.4 (using phase to disambiguate dusk from dawn)
    // Dusk → Night: nf 0.4–1.0
    // Dawn → Day: same curve reversed (treat dawn same as dusk palette)
    const HudPalette* from;
    const HudPalette* to;
    float t;

    if (nf < 0.05f) {
        // Pure day
        out = PAL_DAY;
        return;
    } else if (nf > 0.85f) {
        // Pure night
        out = PAL_NIGHT;
        return;
    } else if (phase == PHASE_DUSK || (phase == PHASE_NIGHT && nf < 0.5f)) {
        // Transition day→dusk→night (evening)
        if (nf < 0.4f) {
            from = &PAL_DAY; to = &PAL_DUSK;
            t = nf / 0.4f;
        } else {
            from = &PAL_DUSK; to = &PAL_NIGHT;
            t = (nf - 0.4f) / 0.6f;
        }
    } else {
        // Dawn or morning — dusk palette for the twilight zone
        if (nf < 0.4f) {
            from = &PAL_DAY; to = &PAL_DUSK;
            t = nf / 0.4f;
        } else {
            from = &PAL_DUSK; to = &PAL_NIGHT;
            t = (nf - 0.4f) / 0.6f;
        }
    }

    // Lerp all fields
    out.bg_r  = _lerp8(from->bg_r,  to->bg_r,  t);
    out.bg_g  = _lerp8(from->bg_g,  to->bg_g,  t);
    out.bg_b  = _lerp8(from->bg_b,  to->bg_b,  t);
    out.bg_a  = _lerp8(from->bg_a,  to->bg_a,  t);
    out.rim_r = _lerp8(from->rim_r, to->rim_r, t);
    out.rim_g = _lerp8(from->rim_g, to->rim_g, t);
    out.rim_b = _lerp8(from->rim_b, to->rim_b, t);
    out.rim_a = _lerp8(from->rim_a, to->rim_a, t);
    out.ink_r = _lerp8(from->ink_r, to->ink_r, t);
    out.ink_g = _lerp8(from->ink_g, to->ink_g, t);
    out.ink_b = _lerp8(from->ink_b, to->ink_b, t);
    out.ink2_r = _lerp8(from->ink2_r, to->ink2_r, t);
    out.ink2_g = _lerp8(from->ink2_g, to->ink2_g, t);
    out.ink2_b = _lerp8(from->ink2_b, to->ink2_b, t);
    out.acc_r = _lerp8(from->acc_r, to->acc_r, t);
    out.acc_g = _lerp8(from->acc_g, to->acc_g, t);
    out.acc_b = _lerp8(from->acc_b, to->acc_b, t);
    out.moss_r = _lerp8(from->moss_r, to->moss_r, t);
    out.moss_g = _lerp8(from->moss_g, to->moss_g, t);
    out.moss_b = _lerp8(from->moss_b, to->moss_b, t);
}

// ================================================================
//  Pixel icons (8x8, drawn inline)
// ================================================================

// Population icon: small square blob with two eyes (8x8 bitmap, 1=filled)
static const uint8_t ICON_POP[8] = {
    0b00000000,  //
    0b01111110,  //  ######
    0b01111110,  //  ######
    0b01011010,  //  # ## #   (eyes)
    0b01111110,  //  ######
    0b01111110,  //  ######
    0b01111110,  //  ######
    0b00000000,  //
};

// Food icon: empty jar with wide lid, flat bottom (8x8)
static const uint8_t ICON_FOOD[8] = {
    0b11111111,  // ######## (wide lid)
    0b00111100,  //   ####   (neck)
    0b01000010,  //  #    #
    0b01000010,  //  #    #
    0b01000010,  //  #    #
    0b01000010,  //  #    #
    0b01000010,  //  #    #
    0b01111110,  //  ######  (flat bottom)
};

static void _draw_icon(Arduino_Canvas* gfx, int x, int y,
                       const uint8_t* bitmap, uint16_t color) {
    for (int row = 0; row < 8; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx->drawPixel(x + col, y + row, color);
            }
        }
    }
}

// ================================================================
//  Sundial separator (tiny 7x7 sundial icon between clusters)
// ================================================================
static void _draw_sundial(Arduino_Canvas* gfx, int x, int y, uint16_t color) {
    // Semicircle base (flat bottom)
    //   ###
    //  #   #
    // #  |  #
    // #######   (base line)
    gfx->drawPixel(x + 2, y,     color);
    gfx->drawPixel(x + 3, y,     color);
    gfx->drawPixel(x + 4, y,     color);
    gfx->drawPixel(x + 1, y + 1, color);
    gfx->drawPixel(x + 5, y + 1, color);
    gfx->drawPixel(x,     y + 2, color);
    gfx->drawPixel(x + 6, y + 2, color);
    gfx->drawPixel(x,     y + 3, color);
    gfx->drawPixel(x + 6, y + 3, color);
    // Base line
    for (int i = 0; i <= 6; i++)
        gfx->drawPixel(x + i, y + 4, color);
    // Gnomon (shadow stick, angled)
    gfx->drawPixel(x + 3, y + 3, color);
    gfx->drawPixel(x + 4, y + 2, color);
    gfx->drawPixel(x + 4, y + 1, color);
}

// ================================================================
//  Pulse dot (sine wave on opacity, ~2s period)
// ================================================================
static void _draw_pulse_dot(Arduino_Canvas* gfx, int cx, int cy,
                            uint16_t bg_565, uint8_t moss_r, uint8_t moss_g, uint8_t moss_b) {
    unsigned long ms = millis();
    // 2-second sine wave (0..1 range for alpha modulation)
    float phase = (ms % 2000) / 2000.0f * 2.0f * 3.14159265f;
    float alpha = 0.4f + 0.6f * (0.5f + 0.5f * sinf(phase));  // range 0.4–1.0

    uint8_t a = (uint8_t)(alpha * 255.0f);
    uint16_t col = _blend_over(moss_r, moss_g, moss_b, a, bg_565);

    // 5px diameter circle (radius 2)
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (dx * dx + dy * dy <= 4) {
                gfx->drawPixel(cx + dx, cy + dy, col);
            }
        }
    }
}

// ================================================================
//  Text drawing with the built-in 5x7 font
// ================================================================

// Draw text and return the advance width in pixels
static int _draw_text(Arduino_Canvas* gfx, int x, int y, const char* str, uint16_t color) {
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(str);
    // Arduino_GFX 5x7 font at size 1: each char = 6px wide (5 + 1 gap)
    int len = 0;
    while (str[len]) len++;
    return len * 6;
}

// Measure text width without drawing
static int _text_width(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len * 6;
}

// ================================================================
//  Cached state
// ================================================================
static unsigned long _last_update_ms = 0;
static int   _target_pop       = 0;
static int   _target_age_days  = 0;
static float _target_food_days = 0.0f;
static ColonyPhase _phase      = PHASE_FOUNDING;

// ================================================================
//  Public API
// ================================================================

void hud_init() {
    _load_founded();
    _anim_pop  = { 0, 0, 0 };
    _anim_days = { 0, 0, 0 };
    _anim_food = { 0, 0, 0 };
}

void hud_draw(Arduino_Canvas* gfx, const Chamber& ch) {
    // Record founding time — only with a reliable clock (NTP/RTC).
    // Re-record if previously stored from simulated fallback clock.
    if (g_tod.unix_time > 1000000) {
        bool have_clock = g_tod.ntp_synced || g_tod.rtc_valid;
        if (!_founded_stored && have_clock) {
            _store_founded(g_tod.unix_time, true);
        } else if (_founded_stored && !_founded_reliable && have_clock) {
            _store_founded(g_tod.unix_time, true);
        }
    }

    unsigned long now = millis();

    // Recompute target values once per second
    if (now - _last_update_ms >= 1000) {
        _last_update_ms = now;
        _target_pop = ch.colony->population + (ch.has_queen && ch.queen_obj.alive ? 1 : 0);
        _target_age_days = _colony_age_days();
        _target_food_days = _food_days_remaining(ch);
        _phase = _get_phase(_target_pop);

        _anim_pop.target  = (float)_target_pop;
        _anim_days.target = (float)_target_age_days;
        _anim_food.target = _target_food_days;
    }

    // Animate values (~33ms per frame at 30fps)
    float dt = 0.033f;
    _update_anim(_anim_pop, dt);
    _update_anim(_anim_days, dt);
    _update_anim(_anim_food, dt);

    // Get current palette
    HudPalette pal;
    _get_palette(pal);

    // --- Draw background strip ---
    // We blend the HUD bg over whatever the renderer drew underneath.
    // For performance, use fillRect with a pre-blended opaque approximation
    // (true alpha compositing per-pixel over the framebuffer would be too slow).
    uint16_t bg_565 = _rgb565(pal.bg_r, pal.bg_g, pal.bg_b);
    // Draw semi-transparent by blending with a darkened version of itself
    // approximating alpha over typical soil backgrounds
    gfx->fillRect(0, 0, SCREEN_W, HUD_STRIP_H, bg_565);

    // Bottom rim line
    uint16_t rim_565 = _rgb565(pal.rim_r, pal.rim_g, pal.rim_b);
    gfx->drawFastHLine(0, HUD_STRIP_H - 1, SCREEN_W, rim_565);

    // --- Prepare colors ---
    uint16_t ink  = _rgb565(pal.ink_r, pal.ink_g, pal.ink_b);
    uint16_t ink2 = _rgb565(pal.ink2_r, pal.ink2_g, pal.ink2_b);
    uint16_t acc  = _rgb565(pal.acc_r, pal.acc_g, pal.acc_b);

    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    int x = HUD_PAD_X;
    int text_y = HUD_TEXT_Y;
    int icon_y = text_y + 1;  // vertically center 8px icon with text
    int dot_y  = text_y + 3;  // center dot vertically with text baseline

    // --- Population cluster ---
    // [icon]  N  LilGuys
    _draw_icon(gfx, x, icon_y, ICON_POP, acc);
    x += 12;

    char buf[16];
    int pop_display = (int)roundf(_anim_pop.current);
    snprintf(buf, sizeof(buf), "%d", pop_display);
    x += _draw_text(gfx, x, text_y, buf, ink);
    x += 5;
    x += _draw_text(gfx, x, text_y, "LilGuys", ink2);

    // --- Sundial separator ---
    x += 10;
    _draw_sundial(gfx, x, text_y + 1, ink2);
    x += 14;

    // --- Colony age cluster ---
    // Day N
    x += _draw_text(gfx, x, text_y, "Day", ink2);
    x += 5;
    int age_display = (int)roundf(_anim_days.current);
    if (age_display < 1) age_display = 1;
    snprintf(buf, sizeof(buf), "%d", age_display);
    x += _draw_text(gfx, x, text_y, buf, ink);

    x += 14;

    // --- Food reserves cluster ---
    // [icon]  N  days food
    _draw_icon(gfx, x, icon_y, ICON_FOOD, acc);
    x += 12;

    int food_display = (int)roundf(_anim_food.current);
    if (food_display < 0) food_display = 0;
    snprintf(buf, sizeof(buf), "%d", food_display);
    x += _draw_text(gfx, x, text_y, buf, ink);
    x += 5;
    x += _draw_text(gfx, x, text_y, "days food", ink2);

    // --- Right-aligned: time + day phase + pulse dot ---
    static const char* DAY_PHASE_LABELS[] = { "night", "dawn", "day", "dusk" };
    const char* phase_str = DAY_PHASE_LABELS[g_tod.phase];
    snprintf(buf, sizeof(buf), "%d:%02d", g_tod.local_hour, g_tod.local_minute);

    int dot_radius = 3;
    int right_edge = SCREEN_W - HUD_PAD_X;
    int dot_cx = right_edge - dot_radius;
    int phase_w = _text_width(phase_str);
    int time_w = _text_width(buf);
    int phase_x = dot_cx - 6 - phase_w;
    int time_x = phase_x - 6 - time_w;

    _draw_text(gfx, time_x, text_y, buf, ink);
    _draw_text(gfx, phase_x, text_y, phase_str, ink2);
    _draw_pulse_dot(gfx, dot_cx, text_y + 4, bg_565,
                    pal.moss_r, pal.moss_g, pal.moss_b);
}
