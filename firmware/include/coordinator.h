/* Coordinator — orchestrates chambers and colony-wide state.
 * Currently single-chamber; structured to support multi-chamber later.
 */
#pragma once
#include "chamber.h"
#include "colony_state.h"
#include "events.h"
#include "transport.h"

enum ModuleRole : uint8_t {
    MODULE_UNCONFIGURED = 0,
    MODULE_QUEEN        = 1,
    MODULE_SATELLITE    = 2,
};

// Topology graph entry — one per known module
struct TopoGraphEntry {
    uint16_t module_id     = 0;
    bool     present       = false;
    int8_t   neighbours[FACE_COUNT] = {-1, -1, -1, -1};  // module index or -1
};

class Coordinator {
public:
    ColonyState colony;
    Chamber     chamber;    // single chamber for now
    ModuleRole  role = MODULE_UNCONFIGURED;

    uint16_t boot_id = 0;              // random, set at init, sent in ANNOUNCE
    uint16_t _last_queen_boot_id = 0;  // satellite: last boot_id from queen

    // Topology graph (queen-authoritative, max 2 modules for now)
    static constexpr int MAX_MODULES = 2;
    TopoGraphEntry topo_graph[MAX_MODULES];
    int            topo_module_count = 0;

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

    // Pending outgoing handoffs (ACK-gated, retried on timeout)
    static constexpr int MAX_PENDING_OUT = 8;
    struct PendingOut {
        LilGuyTransfer payload;
        uint32_t sent_ms;
        uint8_t  retries;
        uint8_t  face;
        bool     active = false;
    };
    PendingOut _pending_out[MAX_PENDING_OUT];
    uint16_t   _handoff_seq = 0;
    void _service_pending_handoffs();

    // Dedup table for incoming handoffs (one seq per face, 0xFFFF = never seen)
    uint16_t _last_seen_seq[FACE_COUNT] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    // Departure visual delay — worker sprite vanishes, then actual transfer happens
    static constexpr uint32_t DEPART_DELAY_MS = 2000;
    void _service_departures(EventBus& bus, uint32_t tick_num);

    // Incoming worker placement (immediate — visual delay is on sender side)
    void _place_arrival(const LilGuyTransfer& t, EventBus& bus, uint32_t tick_num,
                        int* first_idx_per_face);

};
