/* Worker transfer payload for ESP-NOW handoff between modules. */
#pragma once
#include <cstdint>
#include "lil_guy.h"

// Packed transfer struct — sim-meaningful fields only.
// Animation state resets on arrival. ~60 bytes, well within ESP-NOW 250-byte limit.
struct __attribute__((packed)) LilGuyTransfer {
    uint8_t  msg_type;        // TOPO_HANDOFF
    uint16_t sender_id;       // sender module ID
    uint8_t  arrival_face;    // face on RECEIVER the worker enters from
    int8_t   entry_offset;   // offset from center within entry zone (-1, 0, +1)
    // -- sim state --
    uint8_t  state;           // AntState
    uint8_t  role;            // Role
    uint8_t  is_pioneer;
    float    food_carried;
    float    hunger;
    float    speed;
    int8_t   target_x, target_y;
    uint8_t  has_target;
    uint16_t steps_walked;
    uint16_t chamber_steps;
    uint16_t ticks_away;
    uint16_t stall_ticks;
    uint32_t born_at_ms;
    uint32_t lifespan_ms;
    int16_t  idle_ticks_remaining;
    uint8_t  idle_microstate;
    uint8_t  sleeping;
    uint32_t sleep_until_ms;
};

static_assert(sizeof(LilGuyTransfer) <= 250, "LilGuyTransfer exceeds ESP-NOW max payload");

// Serialize a LilGuy into transfer payload
inline void lil_guy_to_transfer(const LilGuy& w, LilGuyTransfer& t,
                                 uint8_t msg_type, uint16_t sender_id,
                                 uint8_t arrival_face, int8_t entry_offset = 0) {
    t.msg_type        = msg_type;
    t.sender_id       = sender_id;
    t.arrival_face    = arrival_face;
    t.entry_offset    = entry_offset;
    t.state           = w.state;
    t.role            = w.role;
    t.is_pioneer      = w.is_pioneer ? 1 : 0;
    t.food_carried    = w.food_carried;
    t.hunger          = w.hunger;
    t.speed           = w.speed;
    t.target_x        = w.target_x;
    t.target_y        = w.target_y;
    t.has_target      = w.has_target ? 1 : 0;
    t.steps_walked    = w.steps_walked;
    t.chamber_steps   = 0;  // reset on transfer
    t.ticks_away      = w.ticks_away;
    t.stall_ticks     = w.stall_ticks;
    t.born_at_ms      = w.born_at_ms;
    t.lifespan_ms     = w.lifespan_ms;
    t.idle_ticks_remaining = w.idle_ticks_remaining;
    t.idle_microstate = w.idle_microstate;
    t.sleeping        = w.sleeping ? 1 : 0;
    t.sleep_until_ms  = w.sleep_until_ms;
}

// Deserialize transfer payload into a LilGuy at a given entry position.
// Resets animation-only fields. Sets facing inward from arrival face.
inline void transfer_to_lil_guy(const LilGuyTransfer& t, LilGuy& w,
                                 float entry_x, float entry_y,
                                 float face_dx, float face_dy) {
    // Position: one cell inside the chamber from the entry, to avoid
    // immediately re-triggering the boundary handoff. face_dx/face_dy
    // point inward, so adding them steps one cell inside.
    w.x = entry_x + 0.5f + face_dx;
    w.y = entry_y + 0.5f + face_dy;
    w.prev_x = w.x;
    w.prev_y = w.y;
    w.facing_dx = face_dx;
    w.facing_dy = face_dy;
    w.last_dx = face_dx;
    w.last_dy = face_dy;

    // Sim state
    w.state           = static_cast<AntState>(t.state);
    w.role            = static_cast<Role>(t.role);
    w.is_pioneer      = t.is_pioneer != 0;
    w.food_carried    = t.food_carried;
    w.hunger          = t.hunger;
    w.speed           = t.speed;
    w.alive           = true;
    w.target_x        = t.target_x;
    w.target_y        = t.target_y;
    w.has_target      = t.has_target != 0;
    w.has_target_cell = false;  // recompute on next tick
    w.steps_walked    = t.steps_walked;
    w.chamber_steps   = 0;
    w.ticks_away      = t.ticks_away;
    w.stall_ticks     = t.stall_ticks;
    w.idle_cooldown   = 0;
    w.born_at_ms      = t.born_at_ms;
    w.lifespan_ms     = t.lifespan_ms;
    w.idle_ticks_remaining = t.idle_ticks_remaining;
    w.idle_repoll_tick = 0;
    w.idle_microstate = t.idle_microstate;
    w.idle_micro_ticks = 0;
    w.sleeping        = t.sleeping != 0;
    w.sleep_until_ms  = t.sleep_until_ms;
    w.sleep_cooldown_ms = 0;

    // Role params (recompute from role)
    w.move_ticks    = Cfg::ROLE_PARAMS[w.role].move_ticks;
    w.sense_radius  = Cfg::ROLE_PARAMS[w.role].sense_radius;
    w.carry_amount  = Cfg::ROLE_PARAMS[w.role].carry_amount;

    // Reset animation-only fields
    w.anim_type            = LG_ANIM_NONE;
    w.anim_remaining_ticks = 0;
    w.interaction_cooldown = 0;
    w.anim_lean_dx         = 0;
    w.anim_lean_dy         = 0;
    w.stack_on             = -1;
    w.stack_hop_remaining  = 0;
    w.topple_depth         = 0;
    w.stack_cooldown_ms    = 0;
    w.zoomie_target        = -1;
    w.zoomie_ticks         = 0;
    w.last_cell_x          = static_cast<int8_t>(entry_x);
    w.last_cell_y          = static_cast<int8_t>(entry_y);
}
