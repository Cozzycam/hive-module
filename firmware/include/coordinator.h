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

    // Tunnel travel delay — workers wait here before appearing in the chamber
    static constexpr int MAX_TUNNEL_PENDING = 8;
    static constexpr uint32_t TUNNEL_TRAVEL_MS = 2000;
    struct TunnelPending {
        LilGuyTransfer transfer;
        uint32_t appear_at_ms;
        bool active = false;
    };
    TunnelPending _tunnel_pending[MAX_TUNNEL_PENDING];
    void _place_arrival(const LilGuyTransfer& t, EventBus& bus, uint32_t tick_num,
                        int* first_idx_per_face);
};
