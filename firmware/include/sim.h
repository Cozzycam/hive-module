/* Single-board sim coordinator. Runs one founding chamber with food.
 * Also handles touch input → food placement. */
#pragma once
#include "chamber.h"
#include "touch.h"

class Sim {
public:
    ColonyState colony;
    Chamber     chamber;
    uint32_t    tick_count = 0;
    bool        food_started = false;

    void init();
    void tick();

    /* Poll for touch input and place food if tapped. Called from
     * main loop (not from tick) so it runs at display rate. */
    void handle_touch();
};
