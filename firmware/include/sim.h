/* Sim — top-level entry point. Owns the Coordinator and EventBus.
 * main.cpp calls Sim::init/tick/handle_touch; rendering reads
 * coordinator.chamber and coordinator.colony. */
#pragma once
#include "coordinator.h"
#include "touch.h"
#include "events.h"

class Sim {
public:
    Coordinator coordinator;
    EventBus    event_bus;
    uint32_t    tick_count = 0;
    uint32_t    selected_conker_id = 0;  // conker ID, 0 = none
    bool        gathering = false;      // finger held — conkers rush to point
    bool        gather_is_exit = false; // true = heading to edge to cross modules
    float       gather_x = 0, gather_y = 0;  // cell coords of gather point

    void init();
    void tick(float dt);
    void handle_touch();
};
