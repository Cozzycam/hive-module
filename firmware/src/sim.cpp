/* Sim — thin shell over Coordinator. Owns EventBus and tick counter.
 * Debug spawning lives here; orchestration lives in Coordinator. */
#include <Arduino.h>
#include "sim.h"
#include "sd_card.h"
#include "journal.h"
#include "time_of_day.h"

void Sim::init() {
    coordinator.init();  // reads role from NVS
    event_bus.init();
    tick_count = 0;

    if (coordinator.is_queen()) {
        // Initialize persistence registry
        PersistenceState ps = coordinator.registry.init();

        // Init journal and bonds after registry (needs SD + time)
        coordinator.journal.init();
        coordinator.bonds.init();

        bool has_manifest = (ps == PERSIST_OK &&
                             coordinator.registry.manifest().colony_id[0] != '\0');

        if (has_manifest) {
            // Case C: SD card with manifest — restore from disk
            coordinator._persist_restore_from_disk();
            coordinator._bond_load();
            tick_count = coordinator.registry.manifest().last_tick;
        } else {
            // Case A (no SD) or Case B (SD, no manifest): fresh colony, queen only
            Chamber& ch = coordinator.chamber;
            coordinator.colony.population = 0;
            coordinator.colony.worker_census = 0;

            // Case B: SD available, no manifest — migrate live colony to disk
            if (ps == PERSIST_OK) {
                coordinator._persist_migrate_live_colony();
            }
        }
    }
    // Satellite: chamber is already empty (no queen, no workers, no brood)
}

void Sim::tick(float dt) {
    tick_count++;
    coordinator.tick(dt, event_bus, tick_count);
}

void Sim::handle_touch() {
    TouchEvent te;
    if (!touch_poll(&te))
        return;

    // If we were gathering (hold), suppress the release tap
    if (gathering) return;

    int tx = te.x;
    int ty = te.y;
    int cx = tx / Cfg::CELL_SIZE;
    int cy = ty / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT)
        return;

    // Hit-test conkers: find nearest within tap radius (pixel distance)
    Chamber& ch = coordinator.chamber;
    int best = -1;
    int best_dist2 = 35 * 35;  // max 35px tap radius
    for (int i = 0; i < ch.conker_count; i++) {
        if (!ch.conkers[i].alive) continue;
        int px = static_cast<int>(ch.conkers[i].x * Cfg::CELL_SIZE);
        int py = static_cast<int>(ch.conkers[i].y * Cfg::CELL_SIZE);
        int dx = tx - px;
        int dy = ty - py;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best = i;
        }
    }

    if (best >= 0) {
        // Tapped a conker — select it
        auto& c = ch.conkers[best];
        selected_conker_id = c.id;
        // A boop perks them up — attention relieves boredom.
        c.needs[NEED_BOREDOM] -= Cfg::BOREDOM_BOOP_RELIEF;
        if (c.needs[NEED_BOREDOM] < 0.0f) c.needs[NEED_BOREDOM] = 0.0f;
        // A boop also rouses a sleeper — the one thing (besides famine) that
        // wakes them. Knock tiredness below the sleep threshold so they don't
        // flop straight back down — a boop earns a little awake time.
        if (c.sleeping) {
            c.sleeping = false;
            c.stack_on = -1;
            if (c.needs[NEED_REST] > 0.6f) c.needs[NEED_REST] = 0.6f;
        }
        // It notices: stop, face the glass, little startle hop.
        // Sleepers were just roused above, so let them react too.
        if (c.state == STATE_IDLE && c.stack_on < 0
                && c.anim_remaining_ticks == 0) {
            c.anim_type = LG_ANIM_NOTICE;
            c.anim_remaining_ticks = 12;
            c.facing_dx = 0.0f; c.facing_dy = 1.0f;  // toward the viewer
            c.last_dx = 0.0f;   c.last_dy = 1.0f;
            c.has_target_cell = false;
        }
        return;
    }

    // Tapped empty space — if selected, just deselect (no food)
    if (selected_conker_id != 0) {
        selected_conker_id = 0;
        return;
    }
    ch.add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);
    Event ev;
    ev.type = EVT_FOOD_TAPPED;
    ev.tick = tick_count;
    ev.food_tapped = {static_cast<int8_t>(cx), static_cast<int8_t>(cy)};
    event_bus.emit(ev);

    JournalEntry je = {};
    je.tick = tick_count;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_FOOD_TAP;
    je.lilguy_id = 0;
    je.food_tap = {static_cast<int8_t>(cx), static_cast<int8_t>(cy), Cfg::TAP_FEED_AMOUNT};
    coordinator.journal.emit(je);
}

// (Finger scent trails removed in v103 — Amber feedback: hold-to-gather is
// the good gesture; the swipe trail was redundant. Press and drag now
// simply moves the gather point with the finger.)
