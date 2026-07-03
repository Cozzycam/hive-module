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
    TOPO_HANDOFF       = 0x10,  // worker transfer (ConkerTransfer payload)
    TOPO_HANDOFF_ACK   = 0x14,  // receiver confirms worker placement
    TOPO_POP_SYNC  = 0x11,  // satellite population broadcast
    TOPO_PHERO_SYNC = 0x12, // boundary pheromone mirror
    TOPO_STATE_SYNC = 0x13, // queen colony state broadcast (tod + stats)
    TOPO_ANNOUNCE   = 0x20, // queen announces chamber assignment to satellite
    TOPO_WIFI_CREDS = 0x22, // queen → satellite: WiFi credentials for solo mode
    TOPO_SET_ROLE   = 0x23, // queen → satellite: assign module role (from app)
    TOPO_SET_TINT   = 0x24, // queen → satellite: floor tint (from app)
    TOPO_GIFT_FOOD  = 0x26, // queen → neighbouring queen: care package across the border
    TOPO_DEATH_SYNC   = 0x15,  // satellite → queen: worker died on satellite
    TOPO_GATHER_SYNC  = 0x16,  // broadcast: finger held, conkers gather
    TOPO_JOURNAL_RELAY = 0x17, // satellite → queen: diary-worthy moment (the journal lives with her)
    TOPO_OTA_ANNOUNCE = 0x30, // queen → satellites: "new firmware, connect to WiFi"
    TOPO_OTA_READY    = 0x31, // queen → satellites: "server at IP:port, download now"
};

struct __attribute__((packed)) TopologyMessage {
    uint8_t  type;
    uint16_t sender_id;
    uint8_t  face;       // sender's face (0=N 1=S 2=W 3=E, matches Face enum)
    uint8_t  channel;    // AP channel (0 = unknown/channel 1 fallback)
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

// True if a POP_SYNC arrived from this face within the last 15s. Only
// satellites broadcast pop sync, so this is proof the neighbour is a
// satellite (a foreign queen never sends one).
bool topology_pop_sync_fresh(Face f);

// Channel management — queen sets the channel, satellites follow
void     topology_set_wifi_channel(uint8_t channel);  // called when WiFi connects
uint8_t  topology_current_channel();                  // current ESP-NOW channel

// Population sync message (satellite → queen)
struct __attribute__((packed)) PopSyncMessage {
    uint8_t  msg_type;   // TOPO_POP_SYNC
    uint16_t sender_id;
    uint16_t population;
    // Gatherer count (added later — receiver checks length for backwards compat)
    uint16_t gatherers;
    // Module role echo (added v94 — receiver checks length for backwards compat)
    uint8_t  role;       // ModuleRole
    // Floor tint echo (added v100 — length-gated like role)
    uint8_t  tint_r, tint_g, tint_b;
    // Garden post vacancy (added v188 — length-gated): 1 = daytime garden
    // with no qualified green thumb present; the queen sends a replacement
    uint8_t  gardener_wanted;
};

// Chamber announcement (queen → satellite on connect)
struct __attribute__((packed)) AnnounceMessage {
    uint8_t  msg_type;       // TOPO_ANNOUNCE
    uint16_t parent_id;      // queen's module ID
    uint8_t  parent_face;    // face on queen this satellite is attached to
    uint16_t your_id;        // satellite's module ID (echo back for confirmation)
    uint8_t  your_home_face; // face on satellite that points toward queen
    uint16_t boot_id;       // random ID generated at queen boot (changes on reboot)
};

// Role assignment (queen → satellite, relayed from app command)
struct __attribute__((packed)) SetRoleMessage {
    uint8_t  msg_type;   // TOPO_SET_ROLE
    uint16_t sender_id;
    uint16_t target_id;  // module that should adopt the role
    uint8_t  role;       // ModuleRole
};

// Floor tint assignment (queen → satellite, relayed from app command)
struct __attribute__((packed)) SetTintMessage {
    uint8_t  msg_type;   // TOPO_SET_TINT
    uint16_t sender_id;
    uint16_t target_id;  // module that should adopt the tint
    uint8_t  r, g, b;    // 0,0,0 = reset to default
};

// WiFi credentials (queen → satellite on connect, for solo mode)
struct __attribute__((packed)) WifiCredsMessage {
    uint8_t  msg_type;       // TOPO_WIFI_CREDS
    uint16_t sender_id;
    char     ssid[32];
    char     pass[64];
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
    uint8_t  weather;        // WeatherCondition enum
    float    temperature_c;
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

// Gather sync message (broadcast: finger held, come to me)
struct __attribute__((packed)) GatherSyncMessage {
    uint8_t  msg_type;    // TOPO_GATHER_SYNC
    uint16_t sender_id;
    uint8_t  active;      // 1 = gathering, 0 = released
    uint8_t  face;        // sender's face toward receiver
};

// Death sync message (satellite → queen: worker died remotely)
struct __attribute__((packed)) DeathSyncMessage {
    uint8_t  msg_type;    // TOPO_DEATH_SYNC
    uint16_t sender_id;
    uint32_t conker_id;
    uint8_t  cause;       // 0=old_age, 1=starved
};

// Handoff ACK message (receiver → sender)
struct __attribute__((packed)) HandoffAck {
    uint8_t  msg_type;    // TOPO_HANDOFF_ACK
    uint16_t acker_id;    // module that received the ant
    uint16_t seq;         // matches seq in ConkerTransfer
};

// Send raw payload to a face's neighbour (for handoff transfers)
bool topology_send_to_face(Face f, const uint8_t* data, int len);

// Handoff receive buffer — coordinator drains these each tick
struct PendingHandoff {
    uint8_t data[250];
    int     len;
};
int  topology_drain_handoffs(PendingHandoff* out, int max_out);

// Handoff ACK receive buffer — coordinator drains these each tick
struct PendingAck {
    uint8_t data[8];
    int     len;
};
int  topology_drain_handoff_acks(PendingAck* out, int max_out);

// Death sync receive buffer — queen drains these each tick
struct PendingDeathSync {
    uint8_t data[16];
    int     len;
};
int  topology_drain_death_syncs(PendingDeathSync* out, int max_out);

// Journal relay (satellite → queen): crop sowings, critter discoveries and
// future satellite story beats reach the colony diary. jtype = JournalType;
// extra carries the type-specific detail (critter kind / plot index).
struct __attribute__((packed)) JournalRelayMessage {
    uint8_t  msg_type;    // TOPO_JOURNAL_RELAY
    uint16_t sender_id;
    uint8_t  jtype;
    uint32_t lilguy_id;
    char     who[16];
    uint8_t  extra;       // per-type detail (critter kind / plot / kind|ctx<<4)
    char     honoree[16]; // crafted memorials
};
struct PendingJournalRelay {
    uint8_t data[48];
    int     len;
};
int  topology_drain_journal_relays(PendingJournalRelay* out, int max_out);

// Gather sync — receiver polls this
bool topology_has_gather(GatherSyncMessage* out);  // returns true + copies, clears flag

// Remote population tracking (queen reads these)
uint16_t topology_remote_population(Face f);
uint16_t topology_remote_gatherers(Face f);
uint8_t  topology_remote_role(Face f);  // 0 = unknown/not yet synced
uint32_t topology_remote_tint(Face f);  // 0xRRGGBB, 0 = none/unknown
bool     topology_remote_gardener_wanted(Face f);  // garden post vacant over there

// Role assignment — satellite reads this
bool topology_has_set_role(SetRoleMessage* out);  // returns true + copies once, then clears

// Tint assignment — satellite reads this
bool topology_has_set_tint(SetTintMessage* out);  // returns true + copies once, then clears

// Boundary pheromone data from neighbour (coordinator applies to local grid)
const BoundaryPheroData& topology_boundary_phero(Face f);

// Queen state sync — satellite reads these
bool     topology_has_state_sync();    // true if received within last 30s
uint32_t topology_state_sync_age_ms(); // millis since last state sync

// Queens own their clock: call with false so a neighbouring queen's
// state broadcast can't overwrite g_tod/weather
void topology_set_accept_state_sync(bool accept);

// Chamber announcement — satellite reads this
bool topology_has_announce(AnnounceMessage* out);  // returns true + copies once, then clears

// WiFi credentials — satellite reads this
bool topology_has_wifi_creds(WifiCredsMessage* out);  // returns true + copies once, then clears

// Care package from a neighbouring queen — the one sanctioned crossing of a
// closed border. Receiver sizes the gift itself (its own daily burn).
struct __attribute__((packed)) GiftFoodMessage {
    uint8_t  msg_type;   // TOPO_GIFT_FOOD
    uint16_t sender_id;
};
bool topology_has_gift_food(GiftFoodMessage* out);  // returns true + copies once, then clears

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

// Serial diagnostic: print full topology state
void topology_status();

// WiFi coexistence: pause heartbeat timeouts during WiFi windows
void topology_set_wifi_active(bool active);
