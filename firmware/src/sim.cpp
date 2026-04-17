/* Single-board sim coordinator. */
#include "sim.h"

void Sim::init() {
    colony = ColonyState();
    chamber.init(&colony, true);
    tick_count = 0;
    food_started = false;
}

void Sim::tick() {
    tick_count++;

    chamber.tick();

    // Aggregate colony-wide stats
    colony.population = chamber.ant_count;

    int foragers = 0;
    for (int i = 0; i < chamber.ant_count; i++) {
        if (chamber.ants[i].state == STATE_TO_FOOD
                || chamber.ants[i].state == STATE_TO_HOME)
            foragers++;
    }
    colony.forager_count = foragers;

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

    // Single-board food replenishment: add food once workers hatch
    if (!food_started && colony.population > 0) {
        food_started = true;
        chamber.add_food(Cfg::GRID_WIDTH / 2, Cfg::GRID_HEIGHT - 5,
                         Cfg::FOOD_REPLENISH_AMOUNT);
    }
    if (food_started && tick_count % Cfg::FOOD_REPLENISH_INTERVAL == 0) {
        chamber.add_food(Cfg::GRID_WIDTH / 2, Cfg::GRID_HEIGHT - 5,
                         Cfg::FOOD_REPLENISH_AMOUNT);
    }
}
