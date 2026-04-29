/* Topology detection — per-face DETECT pins + ESP-NOW handshake.
 * Ported from tools/topology-test/, extended for four faces.
 */
#pragma once
#include <cstdint>
#include "config.h"

// ESP-NOW message types
enum TopoMsgType : uint8_t {
    TOPO_HELLO     = 0x01,
    TOPO_REPLY     = 0x02,
    TOPO_GOODBYE   = 0x03,
    TOPO_HEARTBEAT = 0x04,
    TOPO_HANDOFF   = 0x10,  // worker transfer (LilGuyTransfer payload)
    TOPO_POP_SYNC  = 0x11,  // satellite population broadcast
    TOPO_PHERO_SYNC = 0x12, // boundary pheromone mirror
    TOPO_STATE_SYNC = 0x13, // queen colony state broadcast (tod + stats)
    TOPO_ANNOUNCE   = 0x20, // queen announces chamber assignment to satellite
    TOPO_OTA_ANNOUNCE = 0x30, // queen → satellites: "new firmware, connect to WiFi"
    TOPO_OTA_READY    = 0x31, // queen → satellites: "server at IP:port, download now"
};

struct __attribute__((packed)) TopologyMessage {
    uint8_t  type;
    uint16_t sender_id;
    uint8_t  face;       // sender's face (0=N 1=S 2=W 3=E, matches Face enum)
    uint8_t  reserved;
};

// Per-face connection state
enum FaceLink : uint8_t {
    LINK_IDLE,
    LINK_DETECTED_LOCAL,   // our DETECT went LOW, sent HELLO
    LINK_CONNECTED,
    LINK_ERROR,
};

struct FaceState {
    FaceLink link            = LINK_IDLE;
    uint16_t neighbour_id    = 0;
    uint8_t  neighbour_mac[6] = {};
    bool     peer_added      = false;
    bool     detect_low      = false;
    bool     prev_detect_low = false;
    uint32_t hello_sent_ms   = 0;
    uint8_t  hello_retries   = 0;
    uint32_t last_hb_tx_ms   = 0;
    uint32_t last_hb_rx_ms   = 0;
};

// Neighbour info (read by Coordinator)
struct Neighbour {
    uint16_t module_id     = 0;
    bool     present       = false;
    uint32_t last_seen_ms  = 0;
};

// Callback: called on connect/disconnect
typedef void (*TopologyCallback)(Face face, bool connected, uint16_t module_id);

// Public API
void     topology_init(TopologyCallback cb);  // call after WiFi/NTP init
void     topology_poll();                     // call every 50ms from loop
uint16_t topology_my_id();
const FaceState& topology_face(Face f);
const Neighbour& topology_neighbour(Face f);

// Population sync message (satellite → queen)
struct __attribute__((packed)) PopSyncMessage {
    uint8_t  msg_type;   // TOPO_POP_SYNC
    uint16_t sender_id;
    uint16_t population;
};

// Chamber announcement (queen → satellite on connect)
struct __attribute__((packed)) AnnounceMessage {
    uint8_t  msg_type;       // TOPO_ANNOUNCE
    uint16_t parent_id;      // queen's module ID
    uint8_t  parent_face;    // face on queen this satellite is attached to
    uint16_t your_id;        // satellite's module ID (echo back for confirmation)
    uint8_t  your_home_face; // face on satellite that points toward queen
};

// Colony state broadcast (queen → satellites)
struct __attribute__((packed)) StateSyncMessage {
    uint8_t  msg_type;       // TOPO_STATE_SYNC
    uint16_t sender_id;
    float    night_factor;
    float    day_progress;
    uint8_t  phase;          // DayPhase
    int      local_hour;
    int      local_minute;
};

// Boundary pheromone sync message — uint8_t-encoded values (0-255 maps to 0-20.0)
static constexpr int PHERO_SYNC_MAX_CELLS = 30;  // max(GRID_WIDTH, GRID_HEIGHT)

struct __attribute__((packed)) PheroSyncMessage {
    uint8_t  msg_type;     // TOPO_PHERO_SYNC
    uint16_t sender_id;
    uint8_t  face;         // sender's face
    uint8_t  cell_count;   // GRID_HEIGHT for E/W, GRID_WIDTH for N/S
    uint8_t  data[PHERO_SYNC_MAX_CELLS * 2];  // [home0, food0, home1, food1, ...]
};

// Received boundary pheromone data per face
struct BoundaryPheroData {
    float    home[PHERO_SYNC_MAX_CELLS];
    float    food[PHERO_SYNC_MAX_CELLS];
    uint8_t  cell_count;
    uint32_t last_rx_ms;
};

// Send raw payload to a face's neighbour (for handoff transfers)
bool topology_send_to_face(Face f, const uint8_t* data, int len);

// Handoff receive buffer — coordinator drains these each tick
struct PendingHandoff {
    uint8_t data[250];
    int     len;
};
int  topology_drain_handoffs(PendingHandoff* out, int max_out);

// Remote population tracking (queen reads these)
uint16_t topology_remote_population(Face f);

// Boundary pheromone data from neighbour (coordinator applies to local grid)
const BoundaryPheroData& topology_boundary_phero(Face f);

// Queen state sync — satellite reads these
bool     topology_has_state_sync();    // true if received within last 30s
uint32_t topology_state_sync_age_ms(); // millis since last state sync

// Chamber announcement — satellite reads this
bool topology_has_announce(AnnounceMessage* out);  // returns true + copies once, then clears

// OTA cascade messages
struct __attribute__((packed)) OtaAnnounceMessage {
    uint8_t  msg_type;       // TOPO_OTA_ANNOUNCE
    uint16_t sender_id;
    uint32_t fw_version;
};

struct __attribute__((packed)) OtaReadyMessage {
    uint8_t  msg_type;       // TOPO_OTA_READY
    uint16_t sender_id;
    uint32_t fw_version;
    uint8_t  ip[4];
    uint16_t port;
};

// Broadcast raw data to all ESP-NOW peers
bool topology_broadcast(const uint8_t* data, int len);

// OTA cascade — satellite polls these from main loop
bool topology_ota_check(uint32_t* fw_version);   // true if announce received (clears flag)
bool topology_ota_server(OtaReadyMessage* out);   // true if server ready (clears flag)

// Debug overlay drawing
void topology_draw_overlay(void* gfx_canvas);  // Arduino_Canvas*
