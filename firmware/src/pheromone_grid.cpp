#include "pheromone_grid.h"
#include <cstring>

PheromoneGrid::PheromoneGrid() {
    memset(_home, 0, sizeof(_home));
    memset(_food, 0, sizeof(_food));
}

float PheromoneGrid::home(int x, int y) const {
    if (!_in(x, y)) return 0.0f;
    float v = _home[_idx(x, y)];
    return v > Cfg::SENSE_FLOOR ? v : 0.0f;
}

float PheromoneGrid::food(int x, int y) const {
    if (!_in(x, y)) return 0.0f;
    float v = _food[_idx(x, y)];
    return v > Cfg::SENSE_FLOOR ? v : 0.0f;
}

float PheromoneGrid::raw_home(int x, int y) const {
    if (!_in(x, y)) return 0.0f;
    return _home[_idx(x, y)];
}

float PheromoneGrid::raw_food(int x, int y) const {
    if (!_in(x, y)) return 0.0f;
    return _food[_idx(x, y)];
}

void PheromoneGrid::deposit_home(int x, int y, float amount) {
    if (!_in(x, y)) return;
    int i = _idx(x, y);
    float v = _home[i];
    if (amount > v) v = amount;
    if (v > Cfg::PHEROMONE_MAX) v = Cfg::PHEROMONE_MAX;
    _home[i] = v;
}

void PheromoneGrid::deposit_food(int x, int y, float amount) {
    if (!_in(x, y)) return;
    int i = _idx(x, y);
    float v = _food[i];
    if (amount > v) v = amount;
    if (v > Cfg::PHEROMONE_MAX) v = Cfg::PHEROMONE_MAX;
    _food[i] = v;
}

void PheromoneGrid::decay() {
    for (int i = 0; i < Cfg::GRID_CELLS; i++) {
        float v = _home[i];
        if (v > 0.0f) {
            v *= Cfg::PHEROMONE_GRID_DECAY;
            _home[i] = (v > Cfg::SENSE_FLOOR) ? v : 0.0f;
        }
    }
    for (int i = 0; i < Cfg::GRID_CELLS; i++) {
        float v = _food[i];
        if (v > 0.0f) {
            v *= Cfg::PHEROMONE_GRID_DECAY;
            _food[i] = (v > Cfg::SENSE_FLOOR) ? v : 0.0f;
        }
    }
}
