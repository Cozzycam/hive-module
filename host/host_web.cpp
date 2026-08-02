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
#include "conker.h"        // conker_activity / _short + render_scale (boop overlay)
#include "rng.h"           // g_rng (host_seed reseeds it per install)
#include "time_of_day.h"   // g_tod (host_set_now)
#include "weather.h"       // g_weather (host_set_weather)
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>    // princess export (host_export_princess)
#include <Preferences.h>    // summon staging shim (host_stage_summon)
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

// Boop/selection overlay — highlight ring + a name/activity pill above the
// tapped conker. On hardware this is drawn in main.cpp's loop (not compiled
// into the WASM build), so the app never showed it; redraw it each frame onto
// the same canvas, right after the scene. Mirrors main.cpp:1183 verbatim minus
// the dirty-rect calls (the host blits the whole framebuffer every frame).
void draw_selection_overlay() {
    if (g_sim.selected_conker_id == 0) return;
    Chamber& ch = chamber();
    int sel = -1;
    for (int i = 0; i < ch.conker_count; i++) {
        if (ch.conkers[i].id == g_sim.selected_conker_id) { sel = i; break; }
    }
    if (sel < 0) { g_sim.selected_conker_id = 0; return; }   // no longer in chamber
    Conker& w = ch.conkers[sel];
    if (!w.alive) { g_sim.selected_conker_id = 0; return; }   // died while selected

    Arduino_Canvas* cv = g_gfx;
    int px = (int)(w.x * Cfg::CELL_SIZE);
    int py = (int)(w.y * Cfg::CELL_SIZE);
    int radius = (int)(w.render_scale() * 6.0f);

    cv->drawCircle(px, py, radius, 0xFFFF);
    cv->drawCircle(px, py, radius + 1, 0xFFFF);

    const char* name = w.name[0] ? w.name : "???";
    const char* act  = conker_activity_short(conker_activity(w, ch));
    cv->setTextSize(1);
    cv->setTextWrap(false);
    int nw = 0; for (const char* p = name; *p; p++) nw += 6;
    int aw = 0; for (const char* p = act;  *p; p++) aw += 6;
    int tw = (nw > aw) ? nw : aw;
    const int pill_h = 20;
    int lx = px - tw / 2;
    if (lx < 3) lx = 3;
    if (lx + tw + 3 > 480) lx = 480 - tw - 3;
    int top = py - radius - 3 - pill_h;
    if (top < 2) top = 2;
    cv->fillRoundRect(lx - 3, top, tw + 6, pill_h, 3, 0x0000);
    cv->setCursor(lx + (tw - nw) / 2, top + 2);
    cv->setTextColor(0xFFFF);           // name — bright
    cv->print(name);
    cv->setCursor(lx + (tw - aw) / 2, top + 11);
    cv->setTextColor(0xC618);           // activity — soft grey
    cv->print(act);
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

// Seed the sim's RNGs from this install's unique id (JS derives a uint32 from
// the persisted colony_id). MUST be called before host_boot: g_rng drives every
// conker's personality/lifespan/behaviour, and esp_random names the queen + the
// registry. Without it, g_rng stays at its Rng(1) default and esp_random runs a
// fixed sequence — so every phone founds the IDENTICAL colony (same queen, same
// cast). Physical modules avoid this by seeding g_rng from the hardware RNG in
// main.cpp, which the WASM build doesn't compile.
EMSCRIPTEN_KEEPALIVE
void host_seed(uint32_t seed) {
    g_rng = Rng(seed);
    esp_random_seed(seed ^ 0x5bd1e995u);   // decorrelate names from behaviour
}

// Catch up to real wall-clock time on reopen — like an always-on hardware
// module. Fast-forward the (restored) colony by `away_seconds`, ticking so brood
// hatches/ages and recording the events to the diary. Time-budgeted so it can't
// freeze the phone: a small colony catches up fully in a blink; a big colony
// after a long absence catches up as much as fits in the budget, then lands the
// clock exactly at now. Founded-age is always correct.
EMSCRIPTEN_KEEPALIVE
void host_catchup(double away_seconds) {
    if (away_seconds < 60.0) return;
    const double MAX_AWAY = 30.0 * 86400.0;          // 30-day sanity cap
    if (away_seconds > MAX_AWAY) away_seconds = MAX_AWAY;
    const uint32_t total_ticks = (uint32_t)(away_seconds * 8.0);
    const uint32_t now_unix   = g_tod.unix_time;     // JS set this to real now before boot
    const uint32_t start_unix = now_unix - (uint32_t)away_seconds;
    const double BUDGET_MS = 2500.0;                 // max freeze on reopen
    const double t0 = emscripten_get_now();
    uint32_t t = 0;
    for (; t < total_ticks; t++) {
        g_sim.tick(1.0f / 8.0f);
        g_host_millis += (unsigned long)TICK_MS;
        g_tod.unix_time = start_unix + (uint32_t)(t / 8u);   // ~1s of wall clock per 8 ticks
        if ((t & 7) == 0) {                          // ~once/sim-second: record events to the diary
            Event ev[128];
            int n = g_sim.event_bus.drain(ev, 128);
            if (n > 0) g_sim.coordinator._journal_from_bus_events(ev, n, g_sim.tick_count);
        }
        if ((t & 2047) == 0 && emscripten_get_now() - t0 > BUDGET_MS) { t++; break; }
    }
    g_tod.unix_time = now_unix;                      // land exactly at now regardless of how far we got
    Event ev[128];
    while (g_sim.event_bus.drain(ev, 128) > 0) {}    // drop leftover display-bus animations
    accumulate_events();                             // catch-up hatches → accumulator → pushed to /events
}

EMSCRIPTEN_KEEPALIVE
void host_boot(int ffMinutes) {
    g_gfx = new Arduino_Canvas(320, 480, nullptr);
    g_gfx->begin();
    g_gfx->setRotation(1);   // -> logical 480 x 320

    g_host_millis = 0;
    g_sim.init();            // fresh colony (no SD): lone queen, founding ladder armed
    // host_boot means "a normal colony". incubation_mode is a Chamber member that
    // Sim::init doesn't touch, so without this a boot AFTER host_boot_incubation in
    // the same instance would silently stay a fenced princess room — a colony
    // confined to 11x14 cells. host_boot_incubation sets the flag after calling us.
    chamber().incubation_mode = false;

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
    draw_selection_overlay();       // boop name/activity pill (main.cpp draws this on HW)
    if (!chamber().incubation_mode) {
        hud_draw(g_gfx, chamber());     // same HUD strip the physical module shows
        hud_draw_version(g_gfx);
    }                                   // incubation: the app draws its own slim chrome (time/weather/day)
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
            // A lone princess's boop gives less company than a colony boop (she's
            // always alone, so loneliness should stay a live, tendable need).
            c.needs[NEED_SOCIAL] -= ch.incubation_mode ? Cfg::PRINCESS_SOCIAL_BOOP_RELIEF
                                                        : Cfg::SOCIAL_BOOP_RELIEF;
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

// Gateway coronation: serialize the raised princess for the VPS handoff park.
// The app POSTs this under a one-time claim token; the module's summon_queen
// command fetches it, stages it in NVS, and founds with her after a reset.
static char g_princess_buf[768];
EMSCRIPTEN_KEEPALIVE
int host_export_princess() {
    Chamber& ch = chamber();
    if (!ch.conker_count) { g_princess_buf[0] = '\0'; return 0; }
    Conker& p = ch.conkers[0];
    JsonDocument doc;
    doc["schema"]      = 1;
    doc["name"]        = p.name;
    doc["from_colony"] = g_sim.coordinator.registry.manifest().colony_id;
    doc["tint_seed"]   = p.tint_seed;
    doc["scale_factor"] = p.scale_factor;
    doc["keeper_bond"] = p.keeper_bond;
    doc["lived_ms"]    = p.lived_ms;
    JsonArray pers = doc["personality"].to<JsonArray>();
    for (int i = 0; i < 8; i++) pers.add(p.personality[i]);
    IdentityRecord* rec = g_sim.coordinator.registry.get(p.id);
    if (rec) {
        doc["born_unix"] = rec->born_unix;
        doc["traits"]    = rec->traits;
        doc["catches"]   = rec->catches;
        doc["accessory"] = rec->accessory;
    }
    int len = (int)serializeJson(doc, g_princess_buf, sizeof(g_princess_buf));
    return len;
}
EMSCRIPTEN_KEEPALIVE uintptr_t host_export_princess_ptr() { return (uintptr_t)g_princess_buf; }

// Test-only: stage a summoned queen exactly as the hardware summon_queen
// command does (NVS "handoff"/"queen") — lets the Node/browser harness prove
// the founding-side import without hardware. Call BEFORE host_boot.
EMSCRIPTEN_KEEPALIVE
void host_stage_summon(const char* json) {
    Preferences prefs;
    prefs.begin("handoff", false);
    prefs.putString("queen", json);
    prefs.end();
}

// Test-only: stage the verdant flag exactly as `reset verdant` does — the
// next host_boot enters the empty awaiting chamber instead of founding.
EMSCRIPTEN_KEEPALIVE
void host_set_verdant() {
    Preferences prefs;
    prefs.begin("handoff", false);
    prefs.putBool("verdant", true);
    prefs.end();
}

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
// Recolour a conker herself (feedback #43/#49) — distinct from host_set_tint,
// which colours the FLOOR. Cosmetic only: identity, not power.
EMSCRIPTEN_KEEPALIVE void host_tint_conker(uint32_t id, int tint) {
    g_sim.coordinator.cmd_set_conker_tint(id, (uint8_t)(tint & 0xFF));
}
// Name the colony (feedback #40) — display name only; colony_id is untouched.
EMSCRIPTEN_KEEPALIVE void host_set_colony_title(const char* title) {
    g_sim.coordinator.cmd_set_colony_title(title ? title : "");
}

// The keeper throws a ball to a spot on the floor (display pixels, like host_tap).
// This is the interaction that ISN'T tapping her — see _do_play_ball. Throwing
// wakes a sleeping princess: being invited to play is attention, and a ball she
// slept through would just look broken. (Dormant waking lives in the shared sim.)
EMSCRIPTEN_KEEPALIVE
void host_throw_ball(int tx, int ty) {
    int cx = tx / Cfg::CELL_SIZE, cy = ty / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT) return;
    Chamber& ch = chamber();
    ch.throw_ball(cx, cy);
    if (ch.conker_count > 0) {
        Conker& c = ch.conkers[0];
        if (c.sleeping) {
            c.sleeping = false;
            c.stack_on = -1;
            if (c.needs[NEED_REST] > 0.6f) c.needs[NEED_REST] = 0.6f;
        }
    }
}

// ---- Gateway incubation / princess (tamagotchi) — the app's single raised conker ----
// Boot the colony, then convert it to a single-princess incubation chamber: no
// queen, no founding, one timed egg. incubation_mode makes her queen-like — she
// never dies, only goes dormant when neglected (see conker.cpp).
EMSCRIPTEN_KEEPALIVE
void host_boot_incubation() {
    host_boot(0);                       // reuse the full setup (gfx/renderer/hud/sim.init)
    Chamber& ch = chamber();
    ch.incubation_mode = true;
    ch.has_queen = false;
    ch.queen_obj.alive = false;         // a dead/absent queen never lays (Queen::tick early-returns)
    ch.food_pile_count = 0;
    // A hand-fed pet has no colony food economy — keep the colony "store" full so
    // food_pressure() reads ~0 (never famine). Otherwise a lone conker with an
    // empty store reads as PERMANENT FAMINE, which suppresses sleep and trips
    // cannibalism/hustle behaviours meant for a starving colony. Nothing consumes
    // it in incubation (she eats from keeper piles), so this stays true.
    if (ch.colony) ch.colony->food_store = 1000.0f;
    // No queen until coronation — there is ONLY the princess. host_boot founded a
    // normal colony (which mints a queen name); blank it so nothing reports a
    // phantom queen. The princess (a Conker) becomes the queen when crowned; at
    // that point her name is promoted here.
    g_sim.coordinator.registry.manifest().queen_name[0] = '\0';

    // Exactly ONE princess — hatched (a conker) or still brood (egg/seed), never
    // both, never extras. host_boot just restored the persisted colony (Case C),
    // so on a reload she comes back with her age/name/colour/personality intact.
    // Extra conkers are marked DEAD in the registry and extra brood records are
    // removed so they never restore again (before this guard, every boot added a
    // fresh egg on top of the restored ones — they piled up, one per reload).
    // The still-an-egg case must KEEP the restored brood: an earlier version
    // zeroed brood_count whenever anything was restored, which deleted the egg
    // on any mid-incubation reload and left the nest permanently empty (the
    // registry record survived, so every boot restored-then-wiped it again).
    if (ch.conker_count > 0) {
        // Hatched. Only her — drop extra conkers and any leftover brood.
        for (int i = 1; i < ch.conker_count; i++)
            g_sim.coordinator.registry.mark_dead(ch.conkers[i].id, g_tod.unix_time);
        ch.conker_count = 1;
        for (int i = 0; i < ch.brood_count; i++)
            if (ch.brood[i].id != 0) g_sim.coordinator.registry.remove_brood(ch.brood[i].id);
        ch.brood_count = 0;
    } else if (ch.brood_count > 0) {
        // Still incubating — keep her egg/seed exactly where it left off.
        for (int i = 1; i < ch.brood_count; i++)
            if (ch.brood[i].id != 0) g_sim.coordinator.registry.remove_brood(ch.brood[i].id);
        ch.brood_count = 1;
    } else {
        // Fresh install — found her single egg, in the middle of her room.
        ch.add_brood_with_duration((ch.room_x0() + ch.room_x1()) / 2,
                                   (ch.room_y0() + ch.room_y1()) / 2,
                                   Cfg::INCUBATION_EGG_MS);
    }

    // Pull her inside the room. An install from before the room existed restores
    // her wherever she last stood in the old 30x20 chamber — and _set_target_cell
    // now refuses every step that leaves the room, so a princess restored OUTSIDE
    // it would be walled out and frozen for good, with no way to walk back in.
    for (int i = 0; i < ch.conker_count; i++) {
        Conker& c = ch.conkers[i];
        c.x = ch.clamp_room_x(c.x);
        c.y = ch.clamp_room_y(c.y);
        c.has_target = false;
        c.has_target_cell = false;
    }
    for (int i = 0; i < ch.brood_count; i++) {
        int bx = ch.brood[i].x, by = ch.brood[i].y;
        if (bx < ch.room_x0()) bx = ch.room_x0();
        if (bx > ch.room_x1()) bx = ch.room_x1();
        if (by < ch.room_y0()) by = ch.room_y0();
        if (by > ch.room_y1()) by = ch.room_y1();
        ch.brood[i].x = static_cast<int8_t>(bx);
        ch.brood[i].y = static_cast<int8_t>(by);
    }

    g_events_len = 0;
    accumulate_events();
}

// A keeper feed: DROP a food pile near her — she toddles over and eats it (the
// eating is what deepens the bond, see conker.cpp). Not insta-nourish.
EMSCRIPTEN_KEEPALIVE
void host_feed_princess() {
    Chamber& ch = chamber();
    int fx = Cfg::GRID_WIDTH / 2, fy = Cfg::GRID_HEIGHT / 2;
    if (ch.conker_count > 0) {
        fx = ch.conkers[0].cell_x() + 2;
        fy = ch.conkers[0].cell_y();
        if (fx >= Cfg::GRID_WIDTH - 1) fx = ch.conkers[0].cell_x() - 2;
    }
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;
    if (fx > Cfg::GRID_WIDTH - 2)  fx = Cfg::GRID_WIDTH - 2;
    if (fy > Cfg::GRID_HEIGHT - 2) fy = Cfg::GRID_HEIGHT - 2;
    ch.add_food(fx, fy, Cfg::TAP_FEED_AMOUNT);
}

// Test convenience: fast-forward `seconds` of sim life. fed=1 → an attentive
// keeper feeds ~hourly (watch her grow/mature); fed=0 → neglect (watch her go
// dormant). Lets dormancy be exercised in seconds instead of real days.
EMSCRIPTEN_KEEPALIVE
void host_warp(double seconds, int fed) {
    const uint32_t ticks = (uint32_t)(seconds * 8.0);
    for (uint32_t t = 0; t < ticks; t++) {
        g_sim.tick(1.0f / 8.0f);
        g_host_millis += (unsigned long)TICK_MS;
        if ((t & 7) == 0) {
            g_tod.unix_time += 1;
            // Advance the day/night clock with simulated time so a warp actually
            // cycles day→night (otherwise it stays frozen at the wall clock, and
            // e.g. a night-owl princess never gets a daytime nap → tiredness pegs).
            uint32_t hod = (g_tod.unix_time / 3600u) % 24u;
            g_tod.local_hour   = (uint8_t)hod;
            g_tod.local_minute = (uint8_t)((g_tod.unix_time / 60u) % 60u);
            g_tod.day_of_year  = (uint16_t)((g_tod.unix_time / 86400u) % 365u);
            if (hod >= 7 && hod < 19)       { g_tod.night_factor = 0.0f; g_tod.phase = PHASE_DAY; }
            else if (hod >= 5 && hod < 7)   { g_tod.night_factor = 1.0f - (hod - 5) / 2.0f; g_tod.phase = PHASE_DAWN; }
            else if (hod >= 19 && hod < 21) { g_tod.night_factor = (hod - 19) / 2.0f; g_tod.phase = PHASE_DUSK; }
            else                            { g_tod.night_factor = 1.0f; g_tod.phase = PHASE_NIGHT; }
            Event ev[64];
            int n = g_sim.event_bus.drain(ev, 64);
            if (n > 0) g_sim.coordinator._journal_from_bus_events(ev, n, g_sim.tick_count);
        }
        // "cared for": an attentive keeper drops food only when she's peckish
        // and has nothing to eat — not on a fixed clock (which litters piles).
        if (fed && (t & 511) == 0) {
            Chamber& ch2 = chamber();
            if (ch2.conker_count > 0 && ch2.conkers[0].hunger > 40.0f && ch2.food_pile_count == 0)
                host_feed_princess();
        }
    }
    Event ev[64];
    while (g_sim.event_bus.drain(ev, 64) > 0) {}   // drop leftover display anims
    accumulate_events();
}

// Princess readouts for the test UI (no conker yet → 0s).
EMSCRIPTEN_KEEPALIVE int host_pr_maturity() {   // 0..1000 promille of queen-ready
    Chamber& ch = chamber(); if (!ch.conker_count) return 0;
    uint32_t l = ch.conkers[0].lived_ms, m = Cfg::PRINCESS_READY_MS;
    return l >= m ? 1000 : (int)((uint64_t)l * 1000u / m);
}
EMSCRIPTEN_KEEPALIVE int host_pr_bond()    { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].keeper_bond * 100.0f) : 0; }
EMSCRIPTEN_KEEPALIVE int host_pr_hunger()  { Chamber& ch = chamber(); return ch.conker_count ? (int)ch.conkers[0].hunger : 0; }
EMSCRIPTEN_KEEPALIVE int host_pr_dormant() { Chamber& ch = chamber(); return ch.conker_count ? (ch.conkers[0].dormant ? 1 : 0) : 0; }
EMSCRIPTEN_KEEPALIVE int host_pr_hatched() { return chamber().conker_count > 0 ? 1 : 0; }
// Framebuffer pixel position of the princess — the app camera centres on her.
EMSCRIPTEN_KEEPALIVE int host_pr_px() { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].x * Cfg::CELL_SIZE) : g_gfx->width() / 2; }
EMSCRIPTEN_KEEPALIVE int host_pr_py() { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].y * Cfg::CELL_SIZE) : g_gfx->height() / 2; }
EMSCRIPTEN_KEEPALIVE uint32_t host_founded_unix() { return g_sim.coordinator.registry.manifest().founded_unix; }
EMSCRIPTEN_KEEPALIVE int host_pr_state() { Chamber& ch = chamber(); return ch.conker_count ? (int)ch.conkers[0].state : -1; }
EMSCRIPTEN_KEEPALIVE int host_pr_foodpiles() { return chamber().food_pile_count; }
EMSCRIPTEN_KEEPALIVE int host_pr_ball() { return chamber().has_ball() ? chamber().ball_bounces : 0; }
// Her room in display pixels. The app crops EXACTLY this, so the room's wall and
// the edge of the pane are the same line and there's no camera to chase her with.
EMSCRIPTEN_KEEPALIVE int host_room_x() { return chamber().room_x0() * Cfg::CELL_SIZE; }
EMSCRIPTEN_KEEPALIVE int host_room_y() { return chamber().room_y0() * Cfg::CELL_SIZE; }
EMSCRIPTEN_KEEPALIVE int host_room_w() { Chamber& ch = chamber(); return (ch.room_x1() - ch.room_x0() + 1) * Cfg::CELL_SIZE; }
EMSCRIPTEN_KEEPALIVE int host_room_h() { Chamber& ch = chamber(); return (ch.room_y1() - ch.room_y0() + 1) * Cfg::CELL_SIZE; }
// Ball position in display pixels, -1 when nothing's in play. Exists so a test can
// assert the ball stays inside the Nest's window — the first version threw it out
// of view every time and looked, to a keeper, like the button did nothing.
EMSCRIPTEN_KEEPALIVE int host_pr_ball_x() { Chamber& ch = chamber(); return ch.has_ball() ? (int)(ch.ball_x * Cfg::CELL_SIZE) : -1; }
EMSCRIPTEN_KEEPALIVE int host_pr_ball_y() { Chamber& ch = chamber(); return ch.has_ball() ? (int)(ch.ball_y * Cfg::CELL_SIZE) : -1; }
EMSCRIPTEN_KEEPALIVE int host_pr_sleeping() { Chamber& ch = chamber(); return ch.conker_count ? (ch.conkers[0].sleeping?1:0) : 0; }
// Live needs (0..100), all active per NEEDS_ACTIVE_MASK — the app shows them as bars.
EMSCRIPTEN_KEEPALIVE int host_pr_boredom() { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].needs[NEED_BOREDOM]*100.0f) : 0; }
EMSCRIPTEN_KEEPALIVE int host_pr_social()  { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].needs[NEED_SOCIAL]*100.0f) : 0; }
EMSCRIPTEN_KEEPALIVE int host_pr_rest()    { Chamber& ch = chamber(); return ch.conker_count ? (int)(ch.conkers[0].needs[NEED_REST]*100.0f) : 0; }

}  // extern "C"
