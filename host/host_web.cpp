/* host_web — the firmware as a live queen module in the browser.
 *
 * Now drives the REAL Sim (Coordinator + registry + journal + bonds), not a
 * bare Chamber — so the colony has identities/personalities/diary and produces
 * the exact same snapshot JSON (api_colony_json) that hardware POSTs to the VPS.
 * The phone is literally a queen module: it runs the sim, renders to a canvas,
 * and (next) enrols + pushes that snapshot to the VPS on the same pipeline.
 *
 * JS-facing API:
 *   host_boot(ffMinutes)   build the colony; fast-forward `ffMinutes` of founding
 *   host_step(dt_ms)       advance real-time (8tps fixed step) + render a frame
 *   host_fb/host_w/host_h  framebuffer + logical dims (canvas blit)
 *   host_feed(px,py)       tap-to-feed at a display pixel
 *   host_conkers()         living conker count
 *   host_snapshot()        build api_colony_json into a buffer; returns byte len
 *   host_snapshot_ptr()    pointer to that JSON buffer (read as UTF-8 in JS)
 */
#include "sim.h"
#include "api_json.h"
#include "renderer.h"
#include "events.h"
#include "config.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <emscripten.h>

namespace {
Arduino_Canvas* g_gfx = nullptr;
Sim             g_sim;
Renderer        g_renderer;

double   g_accum_ms = 0.0;
constexpr double TICK_MS = 125.0;   // 8 ticks/sec

char g_snap[96 * 1024];   // snapshot JSON scratch
int  g_snap_len = 0;

Chamber& chamber() { return g_sim.coordinator.chamber; }

void drain_to_renderer() {
    Event drained[128];
    int n = g_sim.event_bus.drain(drained, 128);
    if (n > 0) g_renderer.receive_events(drained, n, chamber());
}
}  // namespace

extern "C" {

// ffMinutes: fast-forward this many minutes of founding so a fresh colony
// already has a starter population (egg0 hatches at 10min, egg1 30min, egg2 1h…).
// A real hardware queen grows the same way in real time; this just skips ahead.
EMSCRIPTEN_KEEPALIVE
void host_boot(int ffMinutes) {
    g_gfx = new Arduino_Canvas(320, 480, nullptr);
    g_gfx->begin();
    g_gfx->setRotation(1);   // -> logical 480 x 320

    g_host_millis = 0;
    g_sim.init();            // fresh colony (no SD): lone queen, founding ladder armed

    if (ffMinutes > 0) {
        unsigned long target = (unsigned long)ffMinutes * 60UL * 1000UL;
        uint32_t guard = 0;
        while (g_host_millis < target && guard < 4000000u) {
            g_sim.tick(1.0f / 8.0f);
            g_host_millis += (unsigned long)TICK_MS;
            guard++;
        }
        // Discard founding-era animation events; the render loop starts clean.
        Event tmp[128];
        while (g_sim.event_bus.drain(tmp, 128) > 0) {}
    }

    g_accum_ms = 0.0;
    g_renderer.init(g_gfx, /*canvas2=*/nullptr, /*output=*/nullptr);
    g_renderer.draw(chamber(), 1.0f);
}

EMSCRIPTEN_KEEPALIVE
void host_step(double dt_ms) {
    if (dt_ms < 0) dt_ms = 0;
    if (dt_ms > 250) dt_ms = 250;   // clamp big pauses (backgrounded tab)
    g_host_millis += (unsigned long)dt_ms;
    g_accum_ms    += dt_ms;
    while (g_accum_ms >= TICK_MS) {
        g_sim.tick(1.0f / 8.0f);
        drain_to_renderer();
        g_accum_ms -= TICK_MS;
    }
    g_renderer.draw(chamber(), (float)(g_accum_ms / TICK_MS));
}

EMSCRIPTEN_KEEPALIVE uintptr_t host_fb() { return (uintptr_t)g_gfx->getFramebuffer(); }
EMSCRIPTEN_KEEPALIVE int host_w() { return g_gfx->width(); }
EMSCRIPTEN_KEEPALIVE int host_h() { return g_gfx->height(); }
EMSCRIPTEN_KEEPALIVE int host_conkers() { return chamber().conker_count; }

EMSCRIPTEN_KEEPALIVE
void host_feed(int px, int py) {
    int cx = px / Cfg::CELL_SIZE, cy = py / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT) return;
    chamber().add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);
    Event ev;
    ev.type = EVT_FOOD_TAPPED;
    ev.tick = g_sim.tick_count;
    ev.food_tapped = { (int8_t)cx, (int8_t)cy };
    g_sim.event_bus.emit(ev);
}

// Build the colony snapshot (the exact JSON hardware POSTs to the VPS).
EMSCRIPTEN_KEEPALIVE
int host_snapshot() {
    g_snap_len = (int)api_colony_json(g_sim.coordinator, g_snap, sizeof(g_snap));
    return g_snap_len;
}
EMSCRIPTEN_KEEPALIVE uintptr_t host_snapshot_ptr() { return (uintptr_t)g_snap; }

}  // extern "C"
