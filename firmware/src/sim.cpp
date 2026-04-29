/* Sim — thin shell over Coordinator. Owns EventBus and tick counter.
 * Debug spawning lives here; orchestration lives in Coordinator. */
#include <Arduino.h>
#include "sim.h"

void Sim::init() {
    coordinator.init();  // reads role from NVS
    event_bus.init();
    tick_count = 0;

    if (coordinator.is_queen()) {
        // Debug: spawn brood + workers for visual/interaction testing
        Chamber& ch = coordinator.chamber;
        int qx = Cfg::QUEEN_SPAWN_X, qy = Cfg::QUEEN_SPAWN_Y;
        ch.add_brood(qx - 1, qy - 3, ROLE_MINOR);
        ch.brood[ch.brood_count - 1].stage = STAGE_EGG;
        ch.add_brood(qx, qy - 3, ROLE_MINOR);
        ch.brood[ch.brood_count - 1].stage = STAGE_LARVA;
        ch.add_brood(qx + 1, qy - 3, ROLE_MINOR);
        ch.brood[ch.brood_count - 1].stage = STAGE_PUPA;
        for (int i = 0; i < 3; i++)
            ch.add_lil_guy(qx - 1 + i, qy - 4, ROLE_MINOR, true);
        for (int i = 0; i < 10; i++)
            ch.add_lil_guy(qx - 5 + i, qy + 3, ROLE_MINOR, false);
        for (int i = 0; i < 3; i++)
            ch.add_lil_guy(qx - 1 + i, qy + 5, ROLE_MAJOR, false);
        coordinator.colony.population = ch.lil_guy_count;
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
    int cx = te.x / Cfg::CELL_SIZE;
    int cy = te.y / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT)
        return;
    coordinator.chamber.add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);
    Event ev;
    ev.type = EVT_FOOD_TAPPED;
    ev.tick = tick_count;
    ev.food_tapped = {static_cast<int8_t>(cx), static_cast<int8_t>(cy)};
    event_bus.emit(ev);
    Serial.printf("[touch] fed (%d,%d) +%.0f\n", cx, cy, Cfg::TAP_FEED_AMOUNT);
}
