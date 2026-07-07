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
#include "hud.h"
#include "names.h"
#include "events.h"
#include "config.h"
#include "time_of_day.h"   // g_tod (host_set_now)
#include "weather.h"       // g_weather (host_set_weather)
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

char   g_events[128 * 1024];   // accumulated diary events (comma-separated JSON objects)
size_t g_events_len = 0;

Chamber& chamber() { return g_sim.coordinator.chamber; }

// Pull any new diary entries out of the journal into the accumulator that JS
// pushes to /events. Called every step and once at boot (so the founding
// history is ready for the very first push, not gated on a render frame).
void accumulate_events() {
    char tmp[16 * 1024];
    size_t n;
    while ((n = g_sim.coordinator.journal.drain_json(tmp, sizeof(tmp))) > 0) {
        if (g_events_len + n + 2 >= sizeof(g_events)) break;
        if (g_events_len > 0) g_events[g_events_len++] = ',';
        memcpy(g_events + g_events_len, tmp, n);
        g_events_len += n;
        g_events[g_events_len] = '\0';
    }
}

void drain_bus() {
    // Drain the display bus ONCE and feed it to both the renderer (animations)
    // and the journal (diary) — main.cpp does the latter on hardware; without
    // it the diary/chronicle/field-guide never fill.
    Event drained[128];
    int n = g_sim.event_bus.drain(drained, 128);
    if (n > 0) {
        g_renderer.receive_events(drained, n, chamber());
        g_sim.coordinator._journal_from_bus_events(drained, n, g_sim.tick_count);
    }
}
}  // namespace

extern "C" {

// ffMinutes: fast-forward this many minutes of founding so a fresh colony
// already has a starter population (egg0 hatches at 10min, egg1 30min, egg2 1h…).
// A real hardware queen grows the same way in real time; this just skips ahead.
// Set the wall clock (unix seconds) — the sim's day/night + founded/now dates
// read g_tod. JS feeds real time so dates aren't at the epoch. Mark the clock
// valid so the HUD's Day-counter + founded logic engage.
EMSCRIPTEN_KEEPALIVE
void host_set_now(double unix_sec) {
    g_tod.unix_time = (uint32_t)unix_sec;
    g_tod.ntp_synced = true;
    g_tod.rtc_valid = true;
}

// Local clock + day/night, fed from the browser's real local time so the HUD
// clock, day-counter, season and the scene's day/night palette are all live.
EMSCRIPTEN_KEEPALIVE
void host_set_clock(int hour, int minute, int day_of_year, double night_factor, int phase) {
    g_tod.local_hour   = hour;
    g_tod.local_minute = minute;
    g_tod.day_of_year  = day_of_year;
    g_tod.night_factor = (float)night_factor;
    g_tod.phase        = (DayPhase)phase;
}

// Real local weather (JS fetches Open-Meteo and maps to a WeatherCondition),
// so the HUD icon + the scene's rain/snow/fog match — like the physical module.
EMSCRIPTEN_KEEPALIVE
void host_set_weather(int condition, double temp_c) {
    g_weather.valid = true;
    g_weather.condition = (WeatherCondition)condition;
    g_weather.temperature_c = (float)temp_c;
}

EMSCRIPTEN_KEEPALIVE
void host_boot(int ffMinutes) {
    g_gfx = new Arduino_Canvas(320, 480, nullptr);
    g_gfx->begin();
    g_gfx->setRotation(1);   // -> logical 480 x 320

    g_host_millis = 0;
    g_sim.init();            // fresh colony (no SD): lone queen, founding ladder armed

    // Give the colony a queen name (a fresh no-SD manifest leaves it blank).
    auto& man = g_sim.coordinator.registry.manifest();
    if (man.queen_name[0] == '\0') name_random(man.queen_name, sizeof(man.queen_name));
    const uint32_t real_now = g_tod.unix_time;   // set by JS before boot

    if (ffMinutes > 0) {
        // Founding happened over the last ffMinutes: advance the clock through
        // it so diary events get realistic, spread-out timestamps (not all
        // stamped "now"), and record bus events to the journal as we go.
        const uint32_t founded = (real_now > (uint32_t)ffMinutes * 60u)
                                 ? real_now - (uint32_t)ffMinutes * 60u : real_now;
        man.founded_unix = founded;
        const unsigned long target = (unsigned long)ffMinutes * 60UL * 1000UL;
        uint32_t t = 0;
        while (g_host_millis < target && t < 4000000u) {
            g_sim.tick(1.0f / 8.0f);
            g_host_millis += (unsigned long)TICK_MS;
            g_tod.unix_time = founded + (uint32_t)(g_host_millis / 1000);
            if ((t & 7) == 0) {   // ~once per sim-second: record bus events to the diary
                Event ev[128];
                int n = g_sim.event_bus.drain(ev, 128);
                if (n > 0) g_sim.coordinator._journal_from_bus_events(ev, n, g_sim.tick_count);
            }
            t++;
        }
        g_tod.unix_time = real_now;
        // Clear leftover display-bus animations; the diary keeps its journal
        // (its ring buffer holds the most recent founding events).
        Event ev[128];
        while (g_sim.event_bus.drain(ev, 128) > 0) {}
    } else if (man.founded_unix == 0) {
        man.founded_unix = real_now;
    }
    g_events_len = 0;

    hud_init();
    hud_set_colony_name(man.queen_name);
    hud_set_founded_unix(man.founded_unix);

    g_accum_ms = 0.0;
    g_renderer.init(g_gfx, /*canvas2=*/nullptr, /*output=*/nullptr);
    g_renderer.draw(chamber(), 1.0f);
    hud_draw(g_gfx, chamber());
    hud_draw_version(g_gfx);

    accumulate_events();   // seed the founding history so the first push carries it
}

EMSCRIPTEN_KEEPALIVE
void host_step(double dt_ms) {
    if (dt_ms < 0) dt_ms = 0;
    if (dt_ms > 250) dt_ms = 250;   // clamp big pauses (backgrounded tab)
    g_host_millis += (unsigned long)dt_ms;
    g_accum_ms    += dt_ms;
    while (g_accum_ms >= TICK_MS) {
        g_sim.tick(1.0f / 8.0f);
        drain_bus();
        g_accum_ms -= TICK_MS;
    }
    accumulate_events();   // new diary events -> accumulator (pushed to /events in JS)
    g_renderer.draw(chamber(), (float)(g_accum_ms / TICK_MS));
    hud_draw(g_gfx, chamber());     // same HUD strip the physical module shows
    hud_draw_version(g_gfx);
}

EMSCRIPTEN_KEEPALIVE uintptr_t host_fb() { return (uintptr_t)g_gfx->getFramebuffer(); }
EMSCRIPTEN_KEEPALIVE int host_w() { return g_gfx->width(); }
EMSCRIPTEN_KEEPALIVE int host_h() { return g_gfx->height(); }
EMSCRIPTEN_KEEPALIVE int host_conkers() { return chamber().conker_count; }

// A tap at a display pixel — mirrors Sim::handle_touch: boop the nearest conker
// (select + startle + relieve boredom), else deselect, else drop food.
EMSCRIPTEN_KEEPALIVE
void host_tap(int tx, int ty) {
    int cx = tx / Cfg::CELL_SIZE, cy = ty / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT) return;
    Chamber& ch = chamber();

    int best = -1, best_d2 = 35 * 35;   // 35px tap radius
    for (int i = 0; i < ch.conker_count; i++) {
        if (!ch.conkers[i].alive) continue;
        int px = (int)(ch.conkers[i].x * Cfg::CELL_SIZE);
        int py = (int)(ch.conkers[i].y * Cfg::CELL_SIZE);
        int dx = tx - px, dy = ty - py, d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }

    if (best >= 0) {
        auto& c = ch.conkers[best];
        g_sim.selected_conker_id = c.id;
        c.needs[NEED_BOREDOM] -= Cfg::BOREDOM_BOOP_RELIEF;
        if (c.needs[NEED_BOREDOM] < 0.0f) c.needs[NEED_BOREDOM] = 0.0f;
        c.afterglow_ticks = Cfg::AFTERGLOW_TICKS;
        if (Cfg::NEEDS_ACTIVE_MASK & (1 << NEED_SOCIAL)) {
            c.needs[NEED_SOCIAL] -= Cfg::SOCIAL_BOOP_RELIEF;
            if (c.needs[NEED_SOCIAL] < 0.0f) c.needs[NEED_SOCIAL] = 0.0f;
        }
        if (c.sleeping) {
            c.sleeping = false; c.stack_on = -1;
            if (c.needs[NEED_REST] > 0.6f) c.needs[NEED_REST] = 0.6f;
        }
        if (c.state == STATE_IDLE && c.stack_on < 0 && c.anim_remaining_ticks == 0) {
            c.anim_type = LG_ANIM_NOTICE;
            c.anim_remaining_ticks = 12;
            c.facing_dx = 0.0f; c.facing_dy = 1.0f;
            c.last_dx = 0.0f;   c.last_dy = 1.0f;
            c.has_target_cell = false;
        }
        return;
    }
    if (g_sim.selected_conker_id != 0) { g_sim.selected_conker_id = 0; return; }

    ch.add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);
    Event ev;
    ev.type = EVT_FOOD_TAPPED;
    ev.tick = g_sim.tick_count;
    ev.food_tapped = { (int8_t)cx, (int8_t)cy };
    g_sim.event_bus.emit(ev);
}

// Press-and-hold to gather: conkers rush to the finger (ring up around it).
EMSCRIPTEN_KEEPALIVE
void host_gather(int px, int py) {
    Chamber& ch = chamber();
    ch.gather_active  = true;
    ch.gather_is_exit = false;
    ch.gather_x = (float)px / Cfg::CELL_SIZE;
    ch.gather_y = (float)py / Cfg::CELL_SIZE;
    g_sim.gathering = true;
}
EMSCRIPTEN_KEEPALIVE
void host_gather_end() { chamber().gather_active = false; g_sim.gathering = false; }

EMSCRIPTEN_KEEPALIVE uint32_t host_selected() { return g_sim.selected_conker_id; }

// Build the colony snapshot (the exact JSON hardware POSTs to the VPS).
EMSCRIPTEN_KEEPALIVE
int host_snapshot() {
    g_snap_len = (int)api_colony_json(g_sim.coordinator, g_snap, sizeof(g_snap));
    return g_snap_len;
}
EMSCRIPTEN_KEEPALIVE uintptr_t host_snapshot_ptr() { return (uintptr_t)g_snap; }

// Accumulated diary events (the /events payload inner array), pushed by JS.
EMSCRIPTEN_KEEPALIVE uintptr_t host_events_ptr() { return (uintptr_t)g_events; }
EMSCRIPTEN_KEEPALIVE int host_events_len() { return (int)g_events_len; }
EMSCRIPTEN_KEEPALIVE void host_events_clear() { g_events_len = 0; g_events[0] = '\0'; }

// ---- Keeper commands, applied straight to the on-phone colony (no VPS round-trip) ----
EMSCRIPTEN_KEEPALIVE void host_grant_wish(uint32_t id) { g_sim.coordinator.cmd_grant_wish(id); }
EMSCRIPTEN_KEEPALIVE void host_feed_colony(double amount) { g_sim.coordinator.cmd_feed_colony((float)amount); }
EMSCRIPTEN_KEEPALIVE void host_care_package() { g_sim.coordinator.cmd_gift_care_package(0); }
EMSCRIPTEN_KEEPALIVE void host_rename(uint32_t id, const char* name) { g_sim.coordinator.cmd_rename_conker(id, name); }
EMSCRIPTEN_KEEPALIVE void host_set_tint(int r, int g, int b) {
    g_sim.coordinator.cmd_set_floor_tint(0, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

}  // extern "C"
