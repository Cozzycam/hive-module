#include "colony_state.h"

float ColonyState::food_pressure() const {
    float scale = Cfg::metabolic_scale_factor(population);
    float daily_burn = Cfg::QUEEN_METABOLISM * Cfg::TICKS_PER_SIM_DAY;
    daily_burn += population * scale
                  * Cfg::CASTE_PARAMS[CASTE_MINOR].metabolism
                  * Cfg::TICKS_PER_SIM_DAY;
    // Brood feeding cost
    if (brood_larva > 0) {
        float feed_interval = 0.5f / Cfg::LARVA_HUNGER_RATE;  // 500 ticks
        float feeds_per_day = static_cast<float>(Cfg::TICKS_PER_SIM_DAY) / feed_interval;
        daily_burn += brood_larva * feeds_per_day * Cfg::LARVA_FEED_AMOUNT;
    }
    float target = daily_burn * Cfg::FOOD_PRESSURE_TARGET_DAYS;
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

float ColonyState::target_forager_fraction() const {
    if (recovery_boost_remaining > 0)
        return Cfg::MAX_FORAGER_FRACTION;
    float pressure = food_pressure();
    return Cfg::MIN_FORAGER_FRACTION
         + (Cfg::MAX_FORAGER_FRACTION - Cfg::MIN_FORAGER_FRACTION) * pressure;
}
