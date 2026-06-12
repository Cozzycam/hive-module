/* Coordinator — orchestrates chambers and colony-wide state.
 * Currently single-chamber; structured to support multi-chamber later.
 */
#pragma once
#include "chamber.h"
#include "colony_state.h"
#include "events.h"
#include "transport.h"
#include "persistence.h"
#include "journal.h"
#include "bonds.h"
#include "world_condition.h"

enum ModuleRole : uint8_t {
    MODULE_UNCONFIGURED = 0,
    MODULE_QUEEN        = 1,
    MODULE_SATELLITE    = 2,   // plain extension chamber
    // Specialised roles (v94+) — all behave as satellites until their
    // behaviours are built; assignable from the app via set_module_role.
    MODULE_GARDEN       = 3,
    MODULE_FOOD_STORE   = 4,
    MODULE_HEART_TREE   = 5,

    MODULE_ROLE_COUNT,
};

// Role name helpers (shared by serial commands, app commands, API JSON)
const char* module_role_str(uint8_t r);
int         module_role_from_str(const char* s);  // -1 if unknown

// Topology graph entry — one per known module
struct TopoGraphEntry {
    uint16_t module_id     = 0;
    bool     present       = false;
    int8_t   neighbours[FACE_COUNT] = {-1, -1, -1, -1};  // module index or -1
};

class Coordinator {
public:
    ColonyState     colony;
    Chamber         chamber;    // single chamber for now
    ConkerRegistry  registry;
    EventJournal    journal;
    BondStore       bonds;
    WorldCondition  world;
    ModuleRole      role = MODULE_UNCONFIGURED;

    uint16_t boot_id = 0;              // random, set at init, sent in ANNOUNCE
    uint16_t _last_queen_boot_id = 0;  // satellite: last boot_id from queen

    // Topology graph (queen-authoritative, max 2 modules for now)
    static constexpr int MAX_MODULES = 2;
    TopoGraphEntry topo_graph[MAX_MODULES];
    int            topo_module_count = 0;

    // Foreign borders: a face where the neighbour is another QUEEN.
    // Two sovereign colonies must not exchange workers — the link stays
    // up politely but the border is closed (no entries, no handoffs).
    bool foreign_face[FACE_COUNT] = {};

    // Reads role from NVS and initializes accordingly.
    void init();
    void tick(float dt, EventBus& bus, uint32_t tick_num);

    bool is_queen() const { return role == MODULE_QUEEN || role == MODULE_UNCONFIGURED; }

    // Topology event handler (called from topology layer on connect/disconnect)
    void on_topology_change(Face face, bool connected, uint16_t module_id);

    // Print topology graph to serial
    void print_topology() const;

    // Write role to NVS (does NOT reboot — caller should).
    static void set_role_nvs(ModuleRole r);

    // Persistence — called from Sim::init() for boot cases
    void _persist_migrate_live_colony();
    void _persist_restore_from_disk();
    void _bond_load();

    // Challenges — called from serial commands
    void challenge_start(uint8_t type, float severity, uint32_t tick_num);
    void challenge_end(uint32_t tick_num);

    // Remote commands — queued by the companion app, applied via VPS poll
    bool cmd_rename_conker(uint32_t id, const char* new_name);
    bool cmd_feed_colony(float amount);
    bool cmd_set_module_role(uint16_t target_id, uint8_t new_role);
    bool cmd_set_floor_tint(uint16_t target_id, uint8_t r, uint8_t g, uint8_t b);

    // Journal — called from main.cpp with drained EventBus events
    void _journal_from_bus_events(const Event* events, int count, uint32_t tick_num);

private:
    void _aggregate_colony_stats();
    void _sync_topology_to_chamber();
    void _check_edge_crossings(EventBus& bus, uint32_t tick_num);
    void _receive_handoffs(EventBus& bus, uint32_t tick_num);
    void _broadcast_population();
    void _broadcast_state();
    void _send_boundary_pheromones(uint32_t tick_num);
    void _apply_boundary_pheromones();
    uint32_t _last_pop_broadcast_ms = 0;
    uint32_t _last_state_broadcast_ms = 0;

    // ANNOUNCE is re-sent periodically while connected (a single lost packet
    // must never leave a border in the wrong state)
    void _send_announce(Face face, uint16_t module_id);
    uint32_t _last_announce_refresh_ms = 0;

    // Pending outgoing handoffs (ACK-gated, retried on timeout)
    static constexpr int MAX_PENDING_OUT = 8;
    struct PendingOut {
        ConkerTransfer payload;
        uint32_t sent_ms;
        uint8_t  retries;
        uint8_t  face;
        bool     active = false;
    };
    PendingOut _pending_out[MAX_PENDING_OUT];
    uint16_t   _handoff_seq = 0;
    void _service_pending_handoffs();

    // Missing worker reconciliation — periodic census check
    static constexpr int MAX_MISSING = 16;
    struct MissingWorker {
        uint32_t id;
        uint32_t missing_since_ms;  // millis() when first noticed missing
        bool     active = false;
    };
    MissingWorker _missing_workers[MAX_MISSING];
    uint32_t _last_reconcile_ms = 0;
    void _reconcile_workers();
    void _respawn_worker(uint32_t id, IdentityRecord* rec);

    void _receive_death_syncs();
    void _send_death_syncs();

    // Dedup table for incoming handoffs (one seq per face, 0xFFFF = never seen)
    uint16_t _last_seen_seq[FACE_COUNT] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    // Departure visual delay — worker sprite vanishes, then actual transfer happens
    static constexpr uint32_t DEPART_DELAY_MS = 2000;
    void _service_departures(EventBus& bus, uint32_t tick_num);

    // Incoming worker placement (immediate — visual delay is on sender side)
    void _place_arrival(const ConkerTransfer& t, EventBus& bus, uint32_t tick_num,
                        int* first_idx_per_face);

    // Bonds
    uint32_t _bond_decay_tick = 0;
    uint32_t _bond_proximity_tick = 0;
    void _bond_tick(uint32_t tick_num);
    void _bond_detect_proximity(uint32_t tick_num);
    void _bond_persist();

    // Traits + WorldCondition
    uint32_t _trait_check_tick = 0;
    void _world_tick();
    void _trait_tick(uint32_t tick_num);

    // Persistence (internal)
    uint32_t _last_persist_flush_ms = 0;
    void _persist_tick(uint32_t tick_num);
    void _persist_assign_new_brood_ids();
    void _persist_process_hatches();
    void _persist_process_deaths();
    void _persist_update_positions();
    void _persist_sync_colony_state(uint32_t tick_num);
};
