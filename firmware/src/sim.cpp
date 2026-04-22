/* Single-board sim coordinator. */
#include <Arduino.h>
#include "sim.h"

// Event type names for serial logging
static const char* EVT_NAMES[] = {
    "interaction_started", "interaction_ended",
    "food_delivered", "food_tapped", "pile_discovered",
    "queen_laid_egg", "young_hatched", "young_died",
    "lil_guy_died", "handoff_incoming", "handoff_outgoing",
};

void Sim::init() {
    colony = ColonyState();
    chamber.init(&colony, true);
    event_bus.init();
    tick_count = 0;
}

void Sim::tick() {
    tick_count++;

    // Bind event bus to chamber for this tick
    chamber.event_bus = &event_bus;
    chamber.tick_num = tick_count;

    chamber.tick();

    // Aggregate colony-wide stats
    colony.population = chamber.lil_guy_count;

    int gatherers = 0;
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        if (chamber.lil_guys[i].state == STATE_TO_FOOD
                || chamber.lil_guys[i].state == STATE_TO_HOME)
            gatherers++;
    }
    colony.gatherer_count = gatherers;

    uint16_t eggs, larvae, pupae;
    chamber.count_brood(eggs, larvae, pupae);
    colony.brood_egg   = eggs;
    colony.brood_larva = larvae;
    colony.brood_pupa  = pupae;

    // food_total = food_store + queen reserves
    float queen_reserves = 0.0f;
    if (chamber.has_queen && chamber.queen_obj.alive)
        queen_reserves = chamber.queen_obj.reserves;
    colony.food_total = colony.food_store + queen_reserves;

    colony.update_recovery_boost();

    // Food is manual only (tap-to-feed)
}

void Sim::handle_touch() {
    TouchEvent te;
    if (!touch_poll(&te))
        return;

    // Convert display pixels (480x320 after rotation) to grid cells.
    int cx = te.x / Cfg::CELL_SIZE;
    int cy = te.y / Cfg::CELL_SIZE;

    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT)
        return;

    chamber.add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);

    // Emit food_tapped event
    Event ev;
    ev.type = EVT_FOOD_TAPPED;
    ev.tick = tick_count;
    ev.food_tapped = {static_cast<int8_t>(cx), static_cast<int8_t>(cy)};
    event_bus.emit(ev);

    Serial.printf("[touch] fed (%d,%d) +%.0f\n", cx, cy, Cfg::TAP_FEED_AMOUNT);
}

void Sim::drain_events() {
    static Event buf[64];
    int n = event_bus.drain(buf, 64);
    for (int i = 0; i < n; i++) {
        int t = buf[i].type;
        if (t >= 0 && t <= 10)
            Serial.printf("[evt t=%6lu] %s\n", buf[i].tick, EVT_NAMES[t]);
    }
}
