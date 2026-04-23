/* Worker lil_guy -- JohnBuffer-inspired marker-following.
 *
 * Movement engine:
 *   Workers have float (x, y) positions and move fractionally each tick
 *   toward a target_cell (int, int). Cell derivation: int(floor(x/y)).
 *   Decision handlers only fire when has_target_cell is false (arrived).
 *
 *   Pheromone deposits happen on cell entry (tracked by last_cell),
 *   not every tick, so trail density matches actual grid traversal.
 */
#pragma once
#include "config.h"
#include <cmath>

// Per-lilguy animation overlay (does not change state machine)
enum LilGuyAnim : uint8_t {
    LG_ANIM_NONE              = 0,
    LG_ANIM_GREETING          = 1,
    LG_ANIM_FOOD_SHARE_GIVER  = 2,
    LG_ANIM_FOOD_SHARE_RECEIVER = 3,
    // Future: LG_ANIM_GROOMING, LG_ANIM_RETINUE_GROOM, etc.
};

// Sprite frame selection — animations can request dedicated sprite art
enum LilGuySpriteFrame : uint8_t {
    LG_FRAME_BASE     = 0,   // default sprite
    LG_FRAME_LEAN     = 1,   // hand-painted lean frame (pending)
    LG_FRAME_CARRYING = 2,   // carrying seed/larva/corpse (future)
    LG_FRAME_GROOMING = 3,   // reserved for grooming brief
    LG_FRAME_DORMANT  = 4,   // reserved for queen hibernation
};

class Chamber;  // forward declaration

struct LilGuy {
    float    x, y, prev_x, prev_y;        // float cell-center coords
    AntState state          = STATE_IDLE;
    int8_t   target_x, target_y;           // high-level task target (queen/brood pos)
    bool     has_target     = false;
    int8_t   target_cell_x, target_cell_y; // next cell to walk toward
    bool     has_target_cell = false;
    uint32_t born_at_ms     = 0;
    bool     alive          = true;

    Role     role           = ROLE_MINOR;
    uint8_t  move_ticks;                   // kept for reference
    uint8_t  sense_radius;
    float    carry_amount;
    float    speed;                        // cells per tick
    uint32_t lifespan_ms;
    bool     is_pioneer     = false;

    float    facing_dx, facing_dy;         // normalized velocity direction
    float    last_dx, last_dy;
    float    food_carried   = 0.0f;

    uint16_t steps_walked   = 0;
    uint16_t ticks_away     = 0;
    uint16_t stall_ticks    = 0;
    uint16_t idle_cooldown  = 0;
    uint16_t chamber_steps  = 0;
    float    hunger         = 0.0f;

    // Idle/rest state
    int16_t  idle_ticks_remaining = 0;  // >0 = truly resting
    int16_t  idle_repoll_tick     = 0;  // countdown to next _pick_task poll
    uint8_t  idle_microstate      = 0;  // 0=hold, 1=drift, 2=reface
    int16_t  idle_micro_ticks     = 0;  // current microstate duration

    // Animation overlay
    uint8_t  anim_type            = LG_ANIM_NONE;
    uint16_t anim_remaining_ticks = 0;
    uint16_t interaction_cooldown = 0;
    int8_t   anim_lean_dx         = 0;   // lean direction toward partner (-1, 0, +1)
    int8_t   anim_lean_dy         = 0;

    int8_t   last_cell_x, last_cell_y;     // for pheromone deposit gating

    // Helper: current grid cell from float position
    int cell_x() const { return static_cast<int>(floorf(x)); }
    int cell_y() const { return static_cast<int>(floorf(y)); }

    void init(int8_t px, int8_t py, Role c = ROLE_MINOR, bool pioneer = false);
    void tick(Chamber& chamber, float dt);

    // Movement methods
    bool _set_target_cell(int cx, int cy, Chamber& ch);
    void _advance_toward_target(Chamber& ch);
    void _on_enter_cell(int cx, int cy, Chamber& ch);

    // Behavior methods
    void _pick_task(Chamber& ch);
    void _do_to_food(Chamber& ch);
    void _do_to_home(Chamber& ch);
    void _do_tend_brood(Chamber& ch);
    void _do_tend_queen(Chamber& ch);
    void _do_idle(Chamber& ch);
    void _tick_idle(Chamber& ch);
    void _pick_idle_microstate(Chamber& ch);
    float _colony_idle_budget(Chamber& ch);
    void _do_cannibalize(Chamber& ch);
    bool _target_still_valid(Chamber& ch);

    // Marker sampling -- returns true and sets out_dx/out_dy if gradient found
    bool _sample_markers(Chamber& ch, bool use_food, int8_t& out_dx, int8_t& out_dy);

    void _step_toward_cell(int tx, int ty, Chamber& ch);
    void _persistent_forward_step(Chamber& ch);
    void _explore_or_wander(Chamber& ch);

    bool _within_sense(int tx, int ty) const {
        int cx = cell_x(), cy = cell_y();
        int d = abs(tx - cx) + abs(ty - cy);
        return d <= sense_radius;
    }
    bool _detects_food_trail(Chamber& ch);
    int  _nearest_hungry_larva(Chamber& ch);   // returns brood index, -1 if none
    int  _least_invested_larva(Chamber& ch);   // returns brood index, -1 if none
    bool _food_pile_adjacent(Chamber& ch, int8_t& out_x, int8_t& out_y);
    bool _nearest_active_entry(Chamber& ch, int max_dist, int exclude_face,
                               int8_t& out_x, int8_t& out_y);
};
