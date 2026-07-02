/* Worker transfer payload for ESP-NOW handoff between modules. */
#pragma once
#include <cstdint>
#include <cstring>
#include "conker.h"

// Packed transfer struct — sim-meaningful fields only.
// Animation state resets on arrival. ~60 bytes, well within ESP-NOW 250-byte limit.
struct __attribute__((packed)) ConkerTransfer {
    uint8_t  msg_type;        // TOPO_HANDOFF
    uint16_t sender_id;       // sender module ID
    uint8_t  arrival_face;    // face on RECEIVER the worker enters from
    int8_t   entry_offset;   // offset from center within entry zone (-1, 0, +1)
    uint16_t seq;            // sender's sequence number for ACK matching
    uint32_t conker_id;      // persistent identity (survives handoff)
    uint8_t  personality[8]; // 0-255 mapped to 0.0-1.0 (compact transport)
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
    uint32_t lived_ms;
    float    scale_factor;
    uint8_t  is_founder;
    int16_t  idle_ticks_remaining;
    uint8_t  idle_microstate;
    uint8_t  sleeping;
    uint32_t sleep_until_ms;
    uint8_t  tint_seed;
    uint8_t  catches;            // critter catches — the Bug Hunter bank
                                 // crosses the tunnel too (v179 fix: crossings
                                 // used to zero it and thrash the title)
    char     name[16];           // display name (truncated to fit payload)
};

static_assert(sizeof(ConkerTransfer) <= 250, "ConkerTransfer exceeds ESP-NOW max payload");

// Serialize a Conker into transfer payload
inline void conker_to_transfer(const Conker& w, ConkerTransfer& t,
                                 uint8_t msg_type, uint16_t sender_id,
                                 uint8_t arrival_face, int8_t entry_offset = 0) {
    t.msg_type        = msg_type;
    t.sender_id       = sender_id;
    t.arrival_face    = arrival_face;
    t.entry_offset    = entry_offset;
    t.seq             = 0;  // caller overrides for ACK matching
    t.conker_id       = w.id;
    for (int i = 0; i < 8; i++)
        t.personality[i] = static_cast<uint8_t>(w.personality[i] * 255.0f);
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
    t.lived_ms        = w.lived_ms;
    t.scale_factor    = w.scale_factor;
    t.is_founder      = w.is_founder ? 1 : 0;
    t.idle_ticks_remaining = w.idle_ticks_remaining;
    t.idle_microstate = w.idle_microstate;
    t.sleeping        = w.sleeping ? 1 : 0;
    t.sleep_until_ms  = w.sleep_until_ms;
    t.tint_seed       = w.tint_seed;
    t.catches         = w.catches;
    memset(t.name, 0, sizeof(t.name));
    strncpy(t.name, w.name, sizeof(t.name) - 1);
}

// Deserialize transfer payload into a Conker at a given entry position.
// Resets animation-only fields. Sets facing inward from arrival face.
inline void transfer_to_conker(const ConkerTransfer& t, Conker& w,
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

    // Identity + personality
    w.id              = t.conker_id;
    for (int i = 0; i < 8; i++)
        w.personality[i] = t.personality[i] / 255.0f;

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
    w.lived_ms        = t.lived_ms;
    w.scale_factor    = t.scale_factor;
    w.is_founder      = t.is_founder != 0;
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
    w.flair_kind           = 0;
    w.flair_ticks          = 0;
    w.flair_casts_used     = 0;
    w.flair_ceremony_done  = false;
    w.tint_seed            = t.tint_seed;
    w.catches              = t.catches;
    memset(w.name, 0, sizeof(w.name));
    strncpy(w.name, t.name, sizeof(w.name) - 1);
    w.arrival_face         = static_cast<int8_t>(t.arrival_face);
    w.arrival_ms           = 0;  // caller sets to millis() after placement
    w.departing            = false;
    w.depart_at_ms         = 0;
    w.depart_face          = -1;
    w.last_cell_x          = static_cast<int8_t>(entry_x);
    w.last_cell_y          = static_cast<int8_t>(entry_y);
}
