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

    void init();
    void tick(float dt);
    void handle_touch();
};
