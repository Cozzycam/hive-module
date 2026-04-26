/* Single-board sim coordinator — real-time lifecycle. */
#include <Arduino.h>
#include "sim.h"

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
    last_lifecycle_ms = millis();

    // Debug: spawn brood + workers for visual/interaction testing
    int qx = Cfg::QUEEN_SPAWN_X, qy = Cfg::QUEEN_SPAWN_Y;
    chamber.add_brood(qx - 1, qy - 3, ROLE_MINOR);
    chamber.brood[chamber.brood_count - 1].stage = STAGE_EGG;
    chamber.add_brood(qx, qy - 3, ROLE_MINOR);
    chamber.brood[chamber.brood_count - 1].stage = STAGE_LARVA;
    chamber.add_brood(qx + 1, qy - 3, ROLE_MINOR);
    chamber.brood[chamber.brood_count - 1].stage = STAGE_PUPA;
    for (int i = 0; i < 3; i++)
        chamber.add_lil_guy(qx - 1 + i, qy - 4, ROLE_MINOR, true);  // 3 pioneers
    for (int i = 0; i < 3; i++)
        chamber.add_lil_guy(qx - 1 + i, qy + 3, ROLE_MINOR, false); // 3 minors
    for (int i = 0; i < 3; i++)
        chamber.add_lil_guy(qx - 1 + i, qy + 5, ROLE_MAJOR, false); // 3 majors
    colony.population = chamber.lil_guy_count;
}

void Sim::tick(float dt) {
    tick_count++;

    chamber.event_bus = &event_bus;
    chamber.tick_num = tick_count;

    // ---- Centralized food drain (real-time) ----
    float burn_this_tick = colony.daily_burn() / Cfg::SECS_PER_DAY * dt;

    // During founding, queen eats from reserves
    if (!chamber.queen_obj.founding_done && chamber.queen_obj.reserves > 0) {
        float queen_portion = Cfg::QUEEN_FOOD_PER_DAY / Cfg::SECS_PER_DAY * dt;
        float from_reserves = fminf(queen_portion, chamber.queen_obj.reserves);
        chamber.queen_obj.reserves -= from_reserves;
        burn_this_tick -= from_reserves;
        if (burn_this_tick < 0) burn_this_tick = 0;
    }

    colony.food_store = fmaxf(0.0f, colony.food_store - burn_this_tick);

    // ---- Credit larva food investment ----
    if (colony.food_store > 0.0f || (!chamber.queen_obj.founding_done && chamber.queen_obj.reserves > 0)) {
        float per_larva = Cfg::LARVA_FOOD_PER_DAY / Cfg::SECS_PER_DAY * dt;
        for (int i = 0; i < chamber.brood_count; i++) {
            if (chamber.brood[i].stage == STAGE_LARVA && chamber.brood[i].alive()) {
                chamber.brood[i].food_invested += per_larva;
            }
        }
    }

    // ---- Chamber tick (movement, behavior, lifecycle events) ----
    chamber.tick(dt);

    // ---- Aggregate colony stats ----
    colony.population = chamber.lil_guy_count;

    int gatherers = 0;
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        auto& w = chamber.lil_guys[i];
        if (w.state == STATE_TO_FOOD
                || (w.state == STATE_TO_HOME && w.food_carried > 0))
            gatherers++;
    }
    colony.gatherer_count = gatherers;

    uint16_t eggs, larvae, pupae;
    chamber.count_brood(eggs, larvae, pupae);
    colony.brood_egg   = eggs;
    colony.brood_larva = larvae;
    colony.brood_pupa  = pupae;

    float queen_reserves = 0.0f;
    if (chamber.has_queen && chamber.queen_obj.alive)
        queen_reserves = chamber.queen_obj.reserves;
    colony.food_total = colony.food_store + queen_reserves;

    colony.update_recovery_boost();
}

void Sim::handle_touch() {
    TouchEvent te;
    if (!touch_poll(&te))
        return;
    int cx = te.x / Cfg::CELL_SIZE;
    int cy = te.y / Cfg::CELL_SIZE;
    if (cx < 0 || cx >= Cfg::GRID_WIDTH || cy < 0 || cy >= Cfg::GRID_HEIGHT)
        return;
    chamber.add_food(cx, cy, Cfg::TAP_FEED_AMOUNT);
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
