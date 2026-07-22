/* Status HUD — horizontal strip overlay at top of 480x320 display.
 *
 * Layout (left to right):
 *   [pop icon] 127 Conkers · Day 47 · [food icon] 9 days food ... growing [pulse dot]
 *
 * Three palette states (day/dusk/night) derived from g_tod.night_factor + phase.
 * Numbers animate toward target over ~200ms (smooth interpolation).
 */
#include "hud.h"
#include "time_of_day.h"
#include "weather.h"
#include "config.h"
#include "pin_config.h"
#include <Preferences.h>
#include <Wire.h>
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

static char _colony_name[25] = {};
static bool _awaiting = false;   // verdant chamber — no colony yet

void hud_set_colony_name(const char* name) {
    strlcpy(_colony_name, name ? name : "", sizeof(_colony_name));
}

void hud_set_awaiting(bool awaiting) {
    _awaiting = awaiting;
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
    // Founding time is the colony's, not the boot's — restore the persisted
    // value so the "Day N" counter survives power cycles. The authoritative
    // value comes from the SD manifest via hud_set_founded_unix() (called just
    // after this in setup); the NVS copy loaded here is a fallback for the gap
    // before the manifest is wired in, and for very first boots.
    // (A genuine new-colony reset clears the NVS keys in main.cpp's reset path.)
    _load_founded();
    _anim_pop  = { 0, 0, 0 };
    _anim_days = { 0, 0, 0 };
    _anim_food = { 0, 0, 0 };
}

void hud_set_founded_unix(uint32_t founded_unix) {
    if (founded_unix == 0) return;  // manifest unknown — keep NVS fallback
    if (founded_unix == _colony_founded_unix) return;
    // The manifest value wins; mirror it into NVS so any future fallback path
    // (and the coordinator's migration read) sees the correct founding time.
    _store_founded(founded_unix, true);
}

void hud_draw(Arduino_Canvas* gfx, const Chamber& ch) {
    // Record founding time — only with a reliable clock (NTP/RTC).
    // Re-record if previously stored from simulated fallback clock.
    // An awaiting (verdant) chamber has no founding to record.
    if (g_tod.unix_time > 1000000 && !_awaiting) {
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
        _target_pop = ch.colony->population;
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

    // --- Draw translucent background strip ---
    // Alpha-blend HUD bg over the rendered scene using the palette's bg_a.
    // 480x28 = 13,440 pixels; ~2-3ms on ESP32-S3 with PSRAM framebuffer.
    //
    // The canvas is 320x480 native (portrait) with setRotation(1) for landscape.
    // Raw FB has stride LCD_WIDTH=320. Rotation 1 maps screen (sx, sy) to
    // native (319-sy, sx), so the HUD strip (screen y 0..27) occupies native
    // columns 292..319 across all 480 native rows. We iterate in native order
    // for cache efficiency.
    uint16_t bg_565 = _rgb565(pal.bg_r, pal.bg_g, pal.bg_b);
    uint16_t* fb = (uint16_t*)gfx->getFramebuffer();
    // The raw-framebuffer alpha blend below assumes the panel's native 320x480
    // portrait layout. The host (phone) build uses a directly-rotated 480x320
    // canvas, so it takes the rotation-safe fillRect path (opaque strip) instead.
#ifdef HOST_BUILD
    const bool _raw_hud = false;
#else
    const bool _raw_hud = true;
#endif
    if (fb && pal.bg_a < 255 && _raw_hud) {
        int a = (pal.bg_a * 257 + 128) >> 8;  // 0-256 fixed-point
        int inv_a = 256 - a;
        int sr5 = (pal.bg_r >> 3) * a;
        int sg6 = (pal.bg_g >> 2) * a;
        int sb5 = (pal.bg_b >> 3) * a;
        int nc_start = LCD_WIDTH - HUD_STRIP_H;  // native col 292
        for (int nr = 0; nr < LCD_HEIGHT; nr++) {
            uint16_t* row = fb + nr * LCD_WIDTH;
            for (int nc = nc_start; nc < LCD_WIDTH; nc++) {
                uint16_t px = row[nc];
                int dr = (px >> 11) & 0x1F;
                int dg = (px >> 5)  & 0x3F;
                int db =  px        & 0x1F;
                int r = (sr5 + dr * inv_a) >> 8;
                int g = (sg6 + dg * inv_a) >> 8;
                int b = (sb5 + db * inv_a) >> 8;
                row[nc] = (r << 11) | (g << 5) | b;
            }
        }
    } else {
        gfx->fillRect(0, 0, SCREEN_W, HUD_STRIP_H, bg_565);
    }

    // Bottom rim line: screen y=HUD_STRIP_H-1 → native col = 320-HUD_STRIP_H = 292
    if (fb && pal.rim_a < 255 && _raw_hud) {
        int a = (pal.rim_a * 257 + 128) >> 8;
        int inv_a = 256 - a;
        int sr5 = (pal.rim_r >> 3) * a;
        int sg6 = (pal.rim_g >> 2) * a;
        int sb5 = (pal.rim_b >> 3) * a;
        int rim_nc = LCD_WIDTH - HUD_STRIP_H;
        for (int nr = 0; nr < LCD_HEIGHT; nr++) {
            int idx = nr * LCD_WIDTH + rim_nc;
            uint16_t px = fb[idx];
            int dr = (px >> 11) & 0x1F;
            int dg = (px >> 5)  & 0x3F;
            int db =  px        & 0x1F;
            int r = (sr5 + dr * inv_a) >> 8;
            int g = (sg6 + dg * inv_a) >> 8;
            int b = (sb5 + db * inv_a) >> 8;
            fb[idx] = (r << 11) | (g << 5) | b;
        }
    } else {
        uint16_t rim_565 = _rgb565(pal.rim_r, pal.rim_g, pal.rim_b);
        gfx->drawFastHLine(0, HUD_STRIP_H - 1, SCREEN_W, rim_565);
    }

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

    char buf[16];
    if (_awaiting) {
        // Verdant chamber — no colony yet. Lead with the module's identity
        // (its colony_id) so a keeper can tell WHICH board to crown from the
        // app's list, then its state. The living world (weather/time, right
        // side) carries on.
        if (_colony_name[0]) {
            x += _draw_text(gfx, x, text_y, _colony_name, acc);
            x += 10;
        }
        _draw_text(gfx, x, text_y, "awaiting a queen", ink2);
    } else {
    // --- Population cluster ---
    // [icon]  N  Conkers
    _draw_icon(gfx, x, icon_y, ICON_POP, acc);
    x += 12;

    int pop_display = (int)roundf(_anim_pop.current);
    snprintf(buf, sizeof(buf), "%d", pop_display);
    x += _draw_text(gfx, x, text_y, buf, ink);
    x += 5;
    x += _draw_text(gfx, x, text_y, "Conkers", ink2);

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

    // --- Colony name, centered — so the module always tells you who it is ---
    if (_colony_name[0]) {
        int cw = _text_width(_colony_name);
        int name_x = (SCREEN_W - cw) / 2;
        if (name_x > x + 10) _draw_text(gfx, name_x, text_y, _colony_name, acc);
    }
    }

    // --- Right-aligned: weather + time + day phase + pulse dot ---
    static const char* DAY_PHASE_LABELS[] = { "night", "dawn", "day", "dusk" };
    static const char* WX_LABELS[] = {
        "clear", "cloudy", "overcast", "fog",
        "drizzle", "rain", "heavy rain", "snow", "storm"
    };

    const char* phase_str = DAY_PHASE_LABELS[g_tod.phase];
    snprintf(buf, sizeof(buf), "%d:%02d", g_tod.local_hour, g_tod.local_minute);

    int dot_radius = 3;
    int right_edge = SCREEN_W - HUD_PAD_X;
    int dot_cx = right_edge - dot_radius;
    int phase_w = _text_width(phase_str);
    int time_w = _text_width(buf);
    int phase_x = dot_cx - 6 - phase_w;
    int time_x = phase_x - 6 - time_w;

    // Weather label (left of time)
    if (g_weather.valid) {
        const char* wx_str = WX_LABELS[g_weather.condition];
        int wx_w = _text_width(wx_str);
        int wx_x = time_x - 8 - wx_w;
        _draw_text(gfx, wx_x, text_y, wx_str, ink2);
    }

    _draw_text(gfx, time_x, text_y, buf, ink);
    _draw_text(gfx, phase_x, text_y, phase_str, ink2);
    // Pulse dot: green = NTP + weather synced, dim grey = no sync
    if (g_tod.ntp_synced && g_weather.valid) {
        _draw_pulse_dot(gfx, dot_cx, text_y + 4, bg_565,
                        pal.moss_r, pal.moss_g, pal.moss_b);
    } else {
        // Static dim dot — not synced
        uint16_t dim = _rgb565(80, 80, 80);
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx * dx + dy * dy <= 4)
                    gfx->drawPixel(dot_cx + dx, text_y + 4 + dy, dim);
    }
}

// ================================================================
//  Battery indicator (AXP2101 PMU)
// ================================================================

// AXP2101 registers (from charge-diagnostic tool)
static constexpr uint8_t AXP_REG_STATUS1 = 0x01;
static constexpr uint8_t AXP_REG_BAT_PCT = 0xA4;

static bool     _axp_found = false;
static int      _batt_pct  = -1;     // 0-100, or -1 if unread
static uint8_t  _charge_state = 0;   // bits [6:5] of STATUS1
static uint32_t _batt_last_read_ms = 0;

static int _axp_read(uint8_t reg) {
    Wire.beginTransmission(AXP2101_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)AXP2101_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

void hud_battery_init() {
    Wire.beginTransmission(AXP2101_ADDR);
    _axp_found = (Wire.endTransmission() == 0);
    if (_axp_found)
        Serial.println("[batt] AXP2101 found");
    else
        Serial.println("[batt] AXP2101 not found — battery indicator disabled");
}

static void _batt_update() {
    if (!_axp_found) return;
    uint32_t now = millis();
    if (now - _batt_last_read_ms < 2000) return;  // read every 2s
    _batt_last_read_ms = now;

    int pct = _axp_read(AXP_REG_BAT_PCT);
    int st1 = _axp_read(AXP_REG_STATUS1);
    if (pct >= 0) _batt_pct = pct > 100 ? 100 : pct;
    if (st1 >= 0) _charge_state = (st1 >> 5) & 0x03;
}

void hud_draw_battery(Arduino_Canvas* gfx) {
    _batt_update();
    if (_batt_pct < 0) return;  // no data yet

    // Get palette for current time of day
    HudPalette pal;
    _get_palette(pal);

    uint16_t outline = _rgb565(pal.ink2_r, pal.ink2_g, pal.ink2_b);
    uint16_t text_col = _rgb565(pal.ink_r, pal.ink_g, pal.ink_b);

    // Fill color based on level
    uint16_t fill;
    if (_batt_pct > 50)
        fill = _rgb565(pal.moss_r, pal.moss_g, pal.moss_b);       // green/moss
    else if (_batt_pct > 20)
        fill = _rgb565(pal.acc_r, pal.acc_g, pal.acc_b);          // amber/accent
    else
        fill = _rgb565(220, 60, 60);                               // red (universal)

    bool charging = (_charge_state == 1);  // 1=charging
    bool charged  = (_charge_state == 2);  // 2=charge done

    // Position: bottom-left corner
    static constexpr int BX = 6;       // left margin
    static constexpr int BY = 308;     // ~12px from bottom of 320
    static constexpr int BW = 16;      // body width
    static constexpr int BH = 9;       // body height
    static constexpr int NW = 2;       // nub width
    static constexpr int NH = 3;       // nub height

    // Battery body outline
    gfx->drawRect(BX, BY, BW, BH, outline);
    // Nub on right
    gfx->fillRect(BX + BW, BY + (BH - NH) / 2, NW, NH, outline);

    // Fill bar inside body (1px padding)
    int max_fill_w = BW - 4;
    int fill_w = (_batt_pct * max_fill_w + 50) / 100;
    if (fill_w < 1 && _batt_pct > 0) fill_w = 1;
    if (fill_w > 0)
        gfx->fillRect(BX + 2, BY + 2, fill_w, BH - 4, fill);

    // Clear unfilled area (in case sim drew something there)
    if (fill_w < max_fill_w)
        gfx->fillRect(BX + 2 + fill_w, BY + 2, max_fill_w - fill_w, BH - 4,
                       _rgb565(pal.bg_r, pal.bg_g, pal.bg_b));

    // Text: always ink color, suffix indicates charge state
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    char buf[12];
    int tx = BX + BW + NW + 4;
    int ty = BY + 1;

    if (charging)
        snprintf(buf, sizeof(buf), "%d%%+", _batt_pct);
    else if (charged)
        snprintf(buf, sizeof(buf), "%d%%*", _batt_pct);
    else
        snprintf(buf, sizeof(buf), "%d%%", _batt_pct);
    _draw_text(gfx, tx, ty, buf, text_col);
}

void hud_draw_version(Arduino_Canvas* gfx) {
    HudPalette pal;
    _get_palette(pal);
    uint16_t col = _rgb565(pal.ink2_r, pal.ink2_g, pal.ink2_b);

    char buf[12];
    snprintf(buf, sizeof(buf), "v%lu", (unsigned long)FW_VERSION);

    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    int w = _text_width(buf);
    _draw_text(gfx, 480 - 6 - w, 309, buf, col);
}
