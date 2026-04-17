/* Two-layer pheromone grid — ported from sim/pheromones.py. */
#pragma once
#include "config.h"

class PheromoneGrid {
public:
    PheromoneGrid();

    float home(int x, int y) const;
    float food(int x, int y) const;
    float raw_home(int x, int y) const;
    float raw_food(int x, int y) const;

    void deposit_home(int x, int y, float amount);
    void deposit_food(int x, int y, float amount);
    void decay();

private:
    float _home[Cfg::GRID_CELLS];
    float _food[Cfg::GRID_CELLS];

    static int  _idx(int x, int y) { return y * Cfg::GRID_WIDTH + x; }
    static bool _in(int x, int y)  {
        return x >= 0 && x < Cfg::GRID_WIDTH && y >= 0 && y < Cfg::GRID_HEIGHT;
    }
};
