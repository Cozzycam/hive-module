/* Desktop yeartest — compiles with g++, no Arduino dependencies.
 * Mirrors headless_yeartest.py to compare C++ port against Python.
 *
 * Build:  g++ -std=c++17 -O2 -I../include -o yeartest desktop_yeartest.cpp \
 *         ../src/colony_state.cpp ../src/pheromone_grid.cpp ../src/brood.cpp \
 *         ../src/lil_guy.cpp ../src/queen.cpp ../src/chamber.cpp ../src/sim.cpp \
 *         ../src/rng.cpp
 */
#include <cstdio>
#include <cstdlib>
#include "config.h"
#include "rng.h"
#include "sim.h"

// No esp_random on desktop — use a fixed seed for reproducibility
Rng g_rng(42);

static const int DAYS = 365;
static const int TICKS = DAYS * Cfg::TICKS_PER_SIM_DAY;
static const int SAMPLE_EVERY = Cfg::TICKS_PER_SIM_DAY;

int main() {
    Sim sim;
    sim.init();

    printf("%5s %5s %8s %8s %4s %4s %4s %4s %6s\n",
           "day", "pop", "store", "total", "eggs", "larv", "pupa", "gthr", "pres");
    printf("------------------------------------------------------\n");

    float peak_food = 0;
    float min_food_post = 999999;
    int peak_pop = 0;

    for (int tick = 1; tick <= TICKS; tick++) {
        sim.tick();

        auto& col = sim.colony;
        int day = tick / Cfg::TICKS_PER_SIM_DAY;

        if (tick % SAMPLE_EVERY == 0) {
            float food = col.food_total;
            int pop = col.population;
            float pressure = col.food_pressure();

            if (food > peak_food) peak_food = food;
            if (day > 56 && food < min_food_post) min_food_post = food;
            if (pop > peak_pop) peak_pop = pop;

            if (day % 5 == 0 || day <= 60) {
                printf("%5d %5d %8.1f %8.1f %4d %4d %4d %4d %6.3f\n",
                    day, pop, col.food_store, col.food_total,
                    col.brood_egg, col.brood_larva, col.brood_pupa,
                    col.gatherer_count, pressure);
            }
        }
    }

    auto& col = sim.colony;
    printf("\n============================================================\n");
    printf("YEAR-END SUMMARY (C++ PORT)\n");
    printf("============================================================\n");
    printf("  Final population:    %d\n", col.population);
    printf("  Peak population:     %d\n", peak_pop);
    printf("  Final food:          %.1f\n", col.food_total);
    printf("  Peak food:           %.1f\n", peak_food);
    printf("  Min food (post-d56): %.1f\n", min_food_post);
    printf("  Final brood:         eggs=%d larvae=%d pupae=%d\n",
           col.brood_egg, col.brood_larva, col.brood_pupa);
    printf("  Food pressure:       %.3f\n", col.food_pressure());
    printf("  Queen alive:         %s\n",
           (sim.chamber.has_queen && sim.chamber.queen_obj.alive) ? "YES" : "NO");
    printf("  Eggs laid total:     %d\n",
           sim.chamber.has_queen ? sim.chamber.queen_obj.eggs_laid : 0);

    printf("\n");
    if (col.population >= 50 && col.population <= 200)
        printf("Population in expected range (50-200). GOOD\n");
    else if (col.population < 50)
        printf("Population %d is BELOW expected range (50-200)\n", col.population);
    else
        printf("Population %d is ABOVE expected range (50-200)\n", col.population);

    if (min_food_post > 20)
        printf("Food never crashed to near-zero post-founding. GOOD\n");
    else
        printf("Food dropped to %.1f post-founding. STILL CRASHING\n", min_food_post);

    return 0;
}
