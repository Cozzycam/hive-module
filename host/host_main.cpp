/* host_main — the render de-risk harness.
 *
 * Builds a Chamber directly (bypassing the Coordinator's networking /
 * persistence / SD layers), seeds it with a small colony, ticks the real
 * sim a few dozen times so the guys settle into life, runs the real firmware
 * Renderer into an Arduino_Canvas, then writes the raw 480x320 RGB565
 * framebuffer to disk. dump_png.js turns that into a viewable PNG.
 *
 * If this produces a coherent colony scene, the whole phone-port thesis holds:
 * the same firmware C++ that drives the LCD can drive a canvas anywhere. */
#include "colony_state.h"
#include "chamber.h"
#include "renderer.h"
#include "events.h"
#include "bonds.h"
#include "rng.h"
#include "config.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <cstdio>

int main() {
    // --- 1. Canvas: constructed 320x480 then rotated to 480x320, exactly as
    //     firmware main.cpp does (Arduino_Canvas(LCD_WIDTH=320, LCD_HEIGHT=480)
    //     + setRotation(1)). ---
    Arduino_Canvas* gfx = new Arduino_Canvas(320, 480, nullptr);
    gfx->begin();
    gfx->setRotation(1);   // -> logical 480 x 320

    // --- 2. Colony + chamber + event bus + friendships ---
    static ColonyState colony;
    static Chamber     chamber;
    static EventBus    bus;
    static BondStore   bonds;
    bus.init();
    bonds.init();
    chamber.init(&colony, /*with_queen=*/true);
    chamber.bonds     = &bonds;
    chamber.event_bus = &bus;
    chamber.tick_num  = 0;

    // Deterministic seed so the frame is reproducible run-to-run.
    g_rng.state = 0x00C0FFEE;

    // --- 3. Populate: a handful of conkers, some brood, a food pile ---
    const int QX = Cfg::QUEEN_SPAWN_X, QY = Cfg::QUEEN_SPAWN_Y;
    for (int i = 0; i < 14; i++) {
        int x = 3 + g_rng.rand_int(0, Cfg::GRID_WIDTH  - 7);
        int y = 2 + g_rng.rand_int(0, Cfg::GRID_HEIGHT - 5);
        chamber.add_conker((int8_t)x, (int8_t)y, ROLE_CONKER, false);
    }
    for (int i = 0; i < 6; i++) {
        int x = QX - 3 + g_rng.rand_int(0, 6);
        int y = QY - 3 + g_rng.rand_int(0, 6);
        chamber.add_brood((int8_t)x, (int8_t)y, ROLE_CONKER);
    }
    chamber.add_food(QX + 5, QY, 40.0f);
    chamber.add_food(QX - 6, QY + 2, 25.0f);

    // --- 4. Tick the real sim ~5s of life at 8 tps so they spread and animate ---
    g_host_millis = 0;
    for (uint32_t t = 1; t <= 40; t++) {
        chamber.tick_num = t;
        chamber.tick(1.0f / 8.0f);
        g_host_millis += 125;   // 8 ticks/sec
    }

    // --- 5. Render one frame through the real firmware Renderer ---
    static Renderer renderer;
    renderer.init(gfx, /*canvas2=*/nullptr, /*output=*/nullptr);
    renderer.draw(chamber, /*lerp_t=*/1.0f);

    // --- 6. Lift the framebuffer straight out and write it to disk ---
    const int W = gfx->width(), H = gfx->height();
    uint16_t* fb = gfx->getFramebuffer();
    FILE* f = fopen("host/out/frame.rgb565", "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot open host/out/frame.rgb565\n"); return 1; }
    fwrite(fb, sizeof(uint16_t), (size_t)W * H, f);
    fclose(f);
    fprintf(stderr, "[host] wrote %dx%d frame, %d conkers\n", W, H, chamber.conker_count);
    return 0;
}
