/* Single-board sim coordinator. Runs one founding chamber with food.
 * Owns the EventBus and passes it to the chamber per-tick. */
#pragma once
#include "chamber.h"
#include "touch.h"
#include "events.h"

class Sim {
public:
    ColonyState colony;
    Chamber     chamber;
    EventBus    event_bus;
    uint32_t    tick_count = 0;
    uint32_t    last_lifecycle_ms = 0;

    void init();
    void tick(float dt);
    void handle_touch();

    /* Drain events and log to serial. Called from main loop at
     * display rate, not sim tick rate. */
    void drain_events();
};
