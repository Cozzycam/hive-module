/* Single-board sim coordinator. Runs one founding chamber with food. */
#pragma once
#include "chamber.h"

class Sim {
public:
    ColonyState colony;
    Chamber     chamber;
    uint32_t    tick_count = 0;
    bool        food_started = false;

    void init();
    void tick();
};
