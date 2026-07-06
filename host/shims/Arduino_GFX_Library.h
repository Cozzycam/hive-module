/* Host shim for <Arduino_GFX_Library.h>.
 *
 * Reimplements the small slice of the Arduino_GFX API the firmware renderer
 * actually calls (~25 methods), backed by a plain uint16_t RGB565 buffer
 * instead of a panel. This is the load-bearing piece of the phone port: the
 * firmware draws its whole scene into an Arduino_Canvas framebuffer, so if
 * this shim reproduces those primitives, host_main can lift the buffer out
 * and paint it anywhere (PNG now; an HTML canvas in the PWA later).
 *
 * Coordinate convention: the firmware constructs the canvas at 320x480 then
 * setRotation(1) to get a 480x320 logical surface (SCREEN_W=480). We store the
 * buffer directly in that rotated, row-major 480x320 space (index = y*W + x),
 * because host_main reads the buffer straight out — it never flushes to a
 * panel, so native panel orientation is irrelevant here.
 *
 * Drawing primitives (circles, rounded rects) follow the Adafruit_GFX /
 * Arduino_GFX algorithms so output matches the device pixel-for-pixel.
 */
#pragma once
#include <Arduino.h>   // on device the real GFX lib pulls this in transitively
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "pgmspace.h"
#include "glcdfont.h"   // the real classic 5x7 CP437 font (vendored from GFX lib)

// Base classes — the firmware passes an Arduino_G*/Arduino_TFT* as the canvas
// "output", but our Canvas ignores it (no panel). They only need to exist.
class Arduino_G {
public:
    virtual ~Arduino_G() {}
};
class Arduino_TFT : public Arduino_G {
public:
    virtual ~Arduino_TFT() {}
};

static inline int16_t _gfx_abs(int16_t v) { return v < 0 ? -v : v; }
static inline void _gfx_swap(int16_t& a, int16_t& b) { int16_t t = a; a = b; b = t; }

class Arduino_Canvas : public Arduino_TFT {
public:
    Arduino_Canvas(int16_t w, int16_t h, Arduino_G* /*output*/ = nullptr,
                   int16_t = 0, int16_t = 0, uint8_t = 0)
        : _raw_w(w), _raw_h(h), _w(w), _h(h) {
        _fb = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
    }
    ~Arduino_Canvas() { free(_fb); }

    bool begin(int32_t = 0) { return _fb != nullptr; }
    uint16_t* getFramebuffer() { return _fb; }
    int16_t width() const { return _w; }
    int16_t height() const { return _h; }
    void flush() {}   // no panel — host_main reads the buffer directly

    // Rotation 1/3 swaps the logical dimensions (odd rotations). The buffer
    // element count is unchanged, so we just reinterpret its stride.
    void setRotation(uint8_t r) {
        if (r & 1) { _w = _raw_h; _h = _raw_w; }
        else       { _w = _raw_w; _h = _raw_h; }
    }

    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    // ---- Core: single pixel (everything else routes through this) ----
    void drawPixel(int16_t x, int16_t y, uint16_t color) {
        if (x < 0 || y < 0 || x >= _w || y >= _h) return;
        _fb[(int)y * _w + x] = color;
    }
    void writePixel(int16_t x, int16_t y, uint16_t color) { drawPixel(x, y, color); }

    // ---- Fills / lines ----
    void fillScreen(uint16_t color) {
        int n = _w * _h;
        for (int i = 0; i < n; i++) _fb[i] = color;
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
        if (w < 0) { x += w + 1; w = -w; }
        for (int16_t i = 0; i < w; i++) drawPixel(x + i, y, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
        if (h < 0) { y += h + 1; h = -h; }
        for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, color);
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        for (int16_t j = 0; j < h; j++) drawFastHLine(x, y + j, w, color);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        drawFastHLine(x, y, w, color);
        drawFastHLine(x, y + h - 1, w, color);
        drawFastVLine(x, y, h, color);
        drawFastVLine(x + w - 1, y, h, color);
    }
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
        int16_t steep = _gfx_abs((int16_t)(y1 - y0)) > _gfx_abs((int16_t)(x1 - x0));
        if (steep) { _gfx_swap(x0, y0); _gfx_swap(x1, y1); }
        if (x0 > x1) { _gfx_swap(x0, x1); _gfx_swap(y0, y1); }
        int16_t dx = x1 - x0, dy = _gfx_abs((int16_t)(y1 - y0));
        int16_t err = dx / 2, ystep = (y0 < y1) ? 1 : -1;
        for (; x0 <= x1; x0++) {
            if (steep) drawPixel(y0, x0, color); else drawPixel(x0, y0, color);
            err -= dy;
            if (err < 0) { y0 += ystep; err += dx; }
        }
    }

    // ---- Circles (Adafruit midpoint algorithm — device-faithful) ----
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        drawPixel(x0, y0 + r, color); drawPixel(x0, y0 - r, color);
        drawPixel(x0 + r, y0, color); drawPixel(x0 - r, y0, color);
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            drawPixel(x0 + x, y0 + y, color); drawPixel(x0 - x, y0 + y, color);
            drawPixel(x0 + x, y0 - y, color); drawPixel(x0 - x, y0 - y, color);
            drawPixel(x0 + y, y0 + x, color); drawPixel(x0 - y, y0 + x, color);
            drawPixel(x0 + y, y0 - x, color); drawPixel(x0 - y, y0 - x, color);
        }
    }
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
        drawFastVLine(x0, y0 - r, 2 * r + 1, color);
        _fillCircleHelper(x0, y0, r, 3, 0, color);
    }

    // ---- Rounded rects ----
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        int16_t maxr = ((w < h) ? w : h) / 2;
        if (r > maxr) r = maxr;
        drawFastHLine(x + r, y, w - 2 * r, color);
        drawFastHLine(x + r, y + h - 1, w - 2 * r, color);
        drawFastVLine(x, y + r, h - 2 * r, color);
        drawFastVLine(x + w - 1, y + r, h - 2 * r, color);
        _drawCircleHelper(x + r, y + r, r, 1, color);
        _drawCircleHelper(x + w - r - 1, y + r, r, 2, color);
        _drawCircleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
        _drawCircleHelper(x + r, y + h - r - 1, r, 8, color);
    }
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        int16_t maxr = ((w < h) ? w : h) / 2;
        if (r > maxr) r = maxr;
        fillRect(x + r, y, w - 2 * r, h, color);
        _fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
        _fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
    }

    // ---- 16-bit bitmap blit (sprites + floor cache rows) ----
    void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w, int16_t h) {
        for (int16_t j = 0; j < h; j++)
            for (int16_t i = 0; i < w; i++)
                drawPixel(x + i, y + j, bitmap[j * w + i]);
    }

    // ---- Text: the real classic 5x7 CP437 font ----
    // Column-major, 5 bytes/glyph; replicates Arduino_GFX's classic drawChar
    // exactly (transparent background, since the firmware only ever calls the
    // single-arg setTextColor). Pixel-identical to the device for zoomie "z"s,
    // bond hearts (\x03), emote glyphs and banners.
    void setTextColor(uint16_t c) { _tcolor = _tbg = c; }
    void setTextColor(uint16_t c, uint16_t bg) { _tcolor = c; _tbg = bg; }
    void setTextSize(uint8_t s) { _tsize = s ? s : 1; }
    void setCursor(int16_t x, int16_t y) { _cx = x; _cy = y; }
    void setTextWrap(bool w) { _twrap = w; }
    size_t write(uint8_t c) {
        if (c == '\n') { _cx = 0; _cy += 8 * _tsize; return 1; }
        if (c == '\r') return 1;
        drawChar(_cx, _cy, c, _tcolor, _tbg, _tsize);
        _cx += 6 * _tsize;
        return 1;
    }
    void drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color,
                  uint16_t bg, uint8_t size) {
        for (int8_t i = 0; i < 5; i++) {          // 5 columns
            uint8_t line = font[c * 5 + i];
            for (int8_t j = 0; j < 8; j++, line >>= 1) {  // 8 rows, LSB=top
                if (line & 1) {
                    if (size == 1) drawPixel(x + i, y + j, color);
                    else fillRect(x + i * size, y + j * size, size, size, color);
                } else if (bg != color) {
                    if (size == 1) drawPixel(x + i, y + j, bg);
                    else fillRect(x + i * size, y + j * size, size, size, bg);
                }
            }
        }
    }
    void print(const char* s) { while (s && *s) write((uint8_t)*s++); }
    void print(char c) { write((uint8_t)c); }
    void print(int v) { char b[16]; snprintf(b, sizeof b, "%d", v); print(b); }
    void print(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v); print(b); }
    void println(const char* s) { print(s); write('\n'); }

private:
    uint16_t* _fb = nullptr;
    int16_t _raw_w, _raw_h, _w, _h;
    uint16_t _tcolor = 0xFFFF;
    uint16_t _tbg = 0xFFFF;
    uint8_t  _tsize = 1;
    int16_t  _cx = 0, _cy = 0;
    bool     _twrap = true;

    void _drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corner, uint16_t color) {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            if (corner & 0x4) { drawPixel(x0 + x, y0 + y, color); drawPixel(x0 + y, y0 + x, color); }
            if (corner & 0x2) { drawPixel(x0 + x, y0 - y, color); drawPixel(x0 + y, y0 - x, color); }
            if (corner & 0x8) { drawPixel(x0 - y, y0 + x, color); drawPixel(x0 - x, y0 + y, color); }
            if (corner & 0x1) { drawPixel(x0 - y, y0 - x, color); drawPixel(x0 - x, y0 - y, color); }
        }
    }
    void _fillCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corner,
                           int16_t delta, uint16_t color) {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        int16_t px = x, py = y;
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            if (x < (y + 1)) {
                if (corner & 1) drawFastVLine(x0 + x, y0 - y, 2 * y + 1 + delta, color);
                if (corner & 2) drawFastVLine(x0 - x, y0 - y, 2 * y + 1 + delta, color);
            }
            if (y != py) {
                if (corner & 1) drawFastVLine(x0 + py, y0 - px, 2 * px + 1 + delta, color);
                if (corner & 2) drawFastVLine(x0 - py, y0 - px, 2 * px + 1 + delta, color);
                py = y;
            }
            px = x;
        }
    }
};

// Panel driver classes referenced by main.cpp (which we don't compile). Declared
// so any stray include resolves; unused in the host build.
class Arduino_DataBus { public: virtual ~Arduino_DataBus() {} };
