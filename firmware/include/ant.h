/* Worker ant — JohnBuffer-inspired marker-following. Ported from sim/ant.py. */
#pragma once
#include "config.h"

class Chamber;  // forward declaration

struct Ant {
    int8_t   x, y, prev_x, prev_y;
    AntState state          = STATE_IDLE;
    int8_t   target_x, target_y;
    bool     has_target     = false;
    uint8_t  move_cooldown  = 0;
    uint32_t age            = 0;
    bool     alive          = true;

    Caste    caste          = CASTE_MINOR;
    uint8_t  move_ticks;
    uint8_t  sense_radius;
    float    carry_amount;
    float    metabolism;
    uint32_t max_age;
    bool     is_nanitic     = false;

    int8_t   facing_dx, facing_dy;
    int8_t   last_dx, last_dy;
    float    food_carried   = 0.0f;

    uint16_t steps_walked   = 0;
    uint16_t ticks_away     = 0;
    uint16_t stall_ticks    = 0;
    uint16_t idle_cooldown  = 0;
    uint16_t chamber_steps  = 0;
    float    hunger         = 0.0f;

    void init(int8_t px, int8_t py, Caste c = CASTE_MINOR, bool nanitic = false);
    void tick(Chamber& chamber);

    // ---- internal methods ----
    void _pick_task(Chamber& ch);
    void _do_to_food(Chamber& ch);
    void _do_to_home(Chamber& ch);
    void _do_tend_brood(Chamber& ch);
    void _do_tend_queen(Chamber& ch);
    void _do_idle(Chamber& ch);
    void _do_cannibalize(Chamber& ch);
    bool _target_still_valid(Chamber& ch);

    // Marker sampling — returns true and sets out_dx/out_dy if gradient found
    bool _sample_markers(Chamber& ch, bool use_food, int8_t& out_dx, int8_t& out_dy);

    void _step_toward_cell(int tx, int ty, Chamber& ch);
    void _persistent_forward_step(Chamber& ch);
    void _explore_or_wander(Chamber& ch);
    bool _try_move(int dx, int dy, Chamber& ch);
    int  _move_delay(Chamber& ch);

    bool _within_sense(int tx, int ty) const {
        int d = abs(tx - x) + abs(ty - y);
        return d <= sense_radius;
    }
    bool _detects_food_trail(Chamber& ch);
    int  _nearest_hungry_larva(Chamber& ch);   // returns brood index, -1 if none
    int  _least_invested_larva(Chamber& ch);   // returns brood index, -1 if none
    bool _food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y);
    bool _nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
                               int8_t& out_x, int8_t& out_y);
};
