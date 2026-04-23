#include "colony_state.h"

float ColonyState::daily_burn() const {
    float scale = Cfg::metabolic_scale_factor(population);
    return Cfg::QUEEN_FOOD_PER_DAY
         + population * Cfg::WORKER_FOOD_PER_DAY * scale
         + brood_larva * Cfg::LARVA_FOOD_PER_DAY;
}

float ColonyState::food_pressure() const {
    float burn = daily_burn();
    float target = burn * Cfg::FOOD_PRESSURE_BUFFER_DAYS;
    if (target <= 0.0f) return 0.5f;
    float ratio = food_total / target;
    float pressure = 1.0f - fminf(1.0f, ratio / 2.0f);
    return fmaxf(0.0f, pressure);
}

void ColonyState::update_recovery_boost() {
    float pressure = food_pressure();
    if (pressure > peak_pressure)
        peak_pressure = pressure;
    if (recovery_boost_remaining > 0) {
        recovery_boost_remaining--;
    } else if (peak_pressure >= Cfg::RECOVERY_BOOST_THRESHOLD
               && pressure < 0.5f) {
        recovery_boost_remaining = Cfg::RECOVERY_BOOST_DURATION;
        peak_pressure = pressure;
    }
}

float ColonyState::target_gatherer_fraction() const {
    if (recovery_boost_remaining > 0)
        return Cfg::MAX_GATHERER_FRACTION;
    float pressure = food_pressure();
    return Cfg::MIN_GATHERER_FRACTION
         + (Cfg::MAX_GATHERER_FRACTION - Cfg::MIN_GATHERER_FRACTION) * pressure;
}
