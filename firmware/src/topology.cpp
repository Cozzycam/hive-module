/* Topology detection — per-face DETECT + ESP-NOW handshake. */
#include "topology.h"
#include "pin_config.h"
#include "time_of_day.h"
#include "weather.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <Arduino_GFX_Library.h>
#include <cstring>

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

// Indexed by Face enum: FACE_N=0, FACE_S=1, FACE_W=2, FACE_E=3
static const uint8_t DETECT_PINS[FACE_COUNT] = { DETECT_N, DETECT_S, DETECT_W, DETECT_E };
static const char*   FACE_NAMES[FACE_COUNT]  = { "N", "S", "W", "E" };

static const uint32_t POLL_MS           = 50;
static const uint32_t HELLO_TIMEOUT_MS  = 1000;
static const uint8_t  HELLO_MAX_RETRIES = 3;
static const uint32_t HEARTBEAT_TX_MS   = 1000;
static const uint32_t HEARTBEAT_RX_TIMEOUT_MS = 3000;

static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

static uint16_t _my_id = 0;
static uint8_t  _my_mac[6];
static FaceState _faces[FACE_COUNT];
static Neighbour _neighbours[FACE_COUNT];
static TopologyCallback _callback = nullptr;
static uint32_t _last_poll_ms = 0;
static uint32_t _reinit_fail_count = 0;
static const uint32_t REINIT_THRESHOLD = 12;  // ~60s of failure (12 × 5s ERROR recovery)
static uint32_t _last_any_connect_ms = 0;     // timestamp of last successful connection
static uint32_t _send_fail_count = 0;         // consecutive esp_now_send failures
static uint32_t _first_send_fail_ms = 0;      // timestamp of first failure in current streak
static uint8_t  _current_channel = 1;         // ESP-NOW operating channel
static uint8_t  _scan_channel = 0;            // channel scanning: 0=disabled, 1-13=trying
static uint8_t  _reinit_count_total = 0;      // total reinits since last connect (for channel scan trigger)
static uint32_t _hello_timeouts_total = 0;    // lifetime HELLO timeout counter
static uint32_t _hb_timeouts_total = 0;       // lifetime heartbeat timeout counter
static bool     _wifi_active = false;         // true during WiFi windows — suppress HB timeout

// Receive ring buffer (ISR -> main loop) — topology messages
static const int RX_BUF_SIZE = 8;
static volatile int _rx_write = 0;
static volatile int _rx_count = 0;
static TopologyMessage _rx_msgs[RX_BUF_SIZE];
static uint8_t         _rx_macs[RX_BUF_SIZE][6];

// Handoff receive buffer (ISR -> coordinator via drain)
static const int HO_BUF_SIZE = 16;
static volatile int _ho_write = 0;
static volatile int _ho_count = 0;
static PendingHandoff _ho_buf[HO_BUF_SIZE];

// Handoff ACK receive buffer (ISR -> coordinator via drain)
static const int ACK_BUF_SIZE = 32;
static volatile int _ack_write = 0;
static volatile int _ack_count = 0;
static PendingAck _ack_buf[ACK_BUF_SIZE];

// Death sync receive buffer (ISR -> coordinator via drain)
static const int DS_BUF_SIZE = 8;
static volatile int _ds_write = 0;
static volatile int _ds_count = 0;
static PendingDeathSync _ds_buf[DS_BUF_SIZE];

// Handoff drop counter (extern, defined in main.cpp)
extern uint32_t g_handoffs_dropped;

// Remote population per face (updated by TOPO_POP_SYNC messages)
static volatile uint16_t _remote_pop[FACE_COUNT] = {0, 0, 0, 0};
// Last POP_SYNC receive time per face — 0 = never. Doubles as satellite-hood
// proof: only satellites broadcast pop sync, a foreign queen never will.
static volatile uint32_t _remote_pop_ms[FACE_COUNT] = {0, 0, 0, 0};
static volatile uint16_t _remote_gatherers[FACE_COUNT] = {0, 0, 0, 0};
static volatile uint8_t  _remote_role[FACE_COUNT] = {0, 0, 0, 0};
static volatile uint32_t _remote_tint[FACE_COUNT] = {0, 0, 0, 0};

// Role assignment (single-shot, set by ISR, read by main loop)
static volatile bool _set_role_pending = false;
static SetRoleMessage _set_role_msg;

// Tint assignment (single-shot, set by ISR, read by main loop)
static volatile bool _set_tint_pending = false;
static SetTintMessage _set_tint_msg;

// Gather sync (volatile, polled by coordinator)
static volatile bool _gather_pending = false;
static GatherSyncMessage _gather_msg;

// Boundary pheromone data from neighbours
static BoundaryPheroData _boundary_phero[FACE_COUNT] = {};

// Queen state sync
static volatile uint32_t _state_sync_last_ms = 0;
static volatile bool _accept_state_sync = true;  // false on queens — they own
                                                 // their clock; a neighbouring
                                                 // queen must not overwrite it

// Chamber announcement (single-shot, cleared after read)
static volatile bool _announce_pending = false;
static AnnounceMessage _announce_msg;

// WiFi credentials (single-shot, set by ISR, read by main loop)
static volatile bool _wifi_creds_pending = false;
static WifiCredsMessage _wifi_creds_msg;

// OTA cascade (single-shot flags, set by ISR, read by main loop)
static volatile bool     _ota_announce_pending = false;
static volatile uint32_t _ota_announce_version = 0;
static volatile bool     _ota_ready_pending = false;
static OtaReadyMessage   _ota_ready_msg;

static constexpr float PHERO_ENCODE_SCALE = 12.75f;  // 255 / 20.0

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static const char* link_str(FaceLink l) {
    switch (l) {
        case LINK_IDLE:           return "IDLE";
        case LINK_DETECTED_LOCAL: return "DETECT";
        case LINK_CONNECTED:      return "CONN";
        case LINK_ERROR:          return "ERR";
        default:                  return "?";
    }
}

static const char* msg_str(uint8_t t) {
    switch (t) {
        case TOPO_HELLO:     return "HELLO";
        case TOPO_REPLY:     return "REPLY";
        case TOPO_GOODBYE:   return "GOODBYE";
        case TOPO_HEARTBEAT: return "HB";
        default:             return "?";
    }
}

static void _send(const uint8_t* mac, uint8_t type, uint8_t face) {
    TopologyMessage msg;
    msg.type      = type;
    msg.sender_id = _my_id;
    msg.face      = face;
    msg.channel   = _current_channel;
    esp_err_t err = esp_now_send(mac, (const uint8_t*)&msg, sizeof(msg));
    if (err != ESP_OK) {
        if (_send_fail_count == 0) _first_send_fail_ms = millis();
        _send_fail_count++;
        if (_send_fail_count == 1) {
            uint8_t actual_ch = 0;
            wifi_second_chan_t second;
            esp_wifi_get_channel(&actual_ch, &second);
            Serial.printf("[topo] send failed (err=0x%x) _ch=%d actual_ch=%d\r\n",
                          err, _current_channel, actual_ch);
        }
        // On channel error, try to fix immediately
        if (err == 0x3069 && !WiFi.isConnected()) {
            esp_wifi_disconnect();
            delay(5);
            esp_wifi_set_channel(_current_channel, WIFI_SECOND_CHAN_NONE);
        }
    } else {
        _send_fail_count = 0;
    }
}

static void _ensure_peer(FaceState& fs, const uint8_t* mac) {
    if (fs.peer_added && memcmp(fs.neighbour_mac, mac, 6) == 0) return;
    if (fs.peer_added) esp_now_del_peer(fs.neighbour_mac);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    memcpy(fs.neighbour_mac, mac, 6);
    fs.peer_added = true;
}

static void _connect_face(Face f, uint16_t id, const uint8_t* mac) {
    FaceState& fs = _faces[f];
    _ensure_peer(fs, mac);
    fs.neighbour_id  = id;
    fs.last_hb_rx_ms = millis();
    fs.last_hb_tx_ms = millis();
    fs.link          = LINK_CONNECTED;

    _neighbours[f].module_id    = id;
    _neighbours[f].present      = true;
    _neighbours[f].last_seen_ms = millis();

    _reinit_fail_count = 0;
    _send_fail_count = 0;
    _reinit_count_total = 0;
    _scan_channel = 0;
    _last_any_connect_ms = millis();
    Serial.printf("[topo] face %s connected to module 0x%04X\r\n", FACE_NAMES[f], id);
    if (_callback) _callback(f, true, id);
}

static void _disconnect_face(Face f, const char* reason) {
    FaceState& fs = _faces[f];
    uint16_t old_id = fs.neighbour_id;
    if (fs.peer_added) {
        esp_now_del_peer(fs.neighbour_mac);
        fs.peer_added = false;
    }
    fs.link         = LINK_IDLE;
    fs.neighbour_id = 0;
    fs.hello_retries = 0;

    _neighbours[f].present = false;
    _neighbours[f].module_id = 0;
    _remote_pop[f] = 0;
    _remote_pop_ms[f] = 0;
    _remote_gatherers[f] = 0;
    _remote_role[f] = 0;
    _remote_tint[f] = 0;
    memset(&_boundary_phero[f], 0, sizeof(BoundaryPheroData));

    Serial.printf("[topo] face %s disconnected (%s)\r\n", FACE_NAMES[f], reason);
    if (_callback && old_id != 0) _callback(f, false, old_id);
}

// ---------------------------------------------------------------------------
//  ESP-NOW receive callback (ISR context)
// ---------------------------------------------------------------------------

static void _on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < 1) return;

    // Route by message type
    uint8_t msg_type = data[0];
    if (msg_type == TOPO_HANDOFF) {
        // Worker transfer — buffer for coordinator
        if (_ho_count >= HO_BUF_SIZE || len > 250) {
            g_handoffs_dropped++;
            return;
        }
        int idx = _ho_write;
        memcpy(_ho_buf[idx].data, data, len);
        _ho_buf[idx].len = len;
        _ho_write = (idx + 1) % HO_BUF_SIZE;
        _ho_count++;
    } else if (msg_type == TOPO_HANDOFF_ACK) {
        // Handoff ACK — buffer for coordinator
        if (_ack_count >= ACK_BUF_SIZE || len > 8) return;
        int idx = _ack_write;
        memcpy(_ack_buf[idx].data, data, len);
        _ack_buf[idx].len = len;
        _ack_write = (idx + 1) % ACK_BUF_SIZE;
        _ack_count++;
    } else if (msg_type == TOPO_PHERO_SYNC && len >= 5) {
        // Boundary pheromone mirror — decode and store
        const PheroSyncMessage* ps = reinterpret_cast<const PheroSyncMessage*>(data);
        // Find which local face this sender is connected to
        for (int f = 0; f < FACE_COUNT; f++) {
            if (_faces[f].link == LINK_CONNECTED && _faces[f].neighbour_id == ps->sender_id) {
                int n = ps->cell_count;
                if (n > PHERO_SYNC_MAX_CELLS) n = PHERO_SYNC_MAX_CELLS;
                BoundaryPheroData& bd = _boundary_phero[f];
                bd.cell_count = n;
                bd.last_rx_ms = millis();
                for (int c = 0; c < n; c++) {
                    bd.home[c] = ps->data[c * 2]     / PHERO_ENCODE_SCALE;
                    bd.food[c] = ps->data[c * 2 + 1] / PHERO_ENCODE_SCALE;
                }
                break;
            }
        }
    } else if (msg_type == TOPO_ANNOUNCE && len >= (int)sizeof(AnnounceMessage)) {
        memcpy(&_announce_msg, data, sizeof(AnnounceMessage));
        _announce_pending = true;
    } else if (msg_type == TOPO_WIFI_CREDS && len >= (int)sizeof(WifiCredsMessage)) {
        memcpy(&_wifi_creds_msg, data, sizeof(WifiCredsMessage));
        _wifi_creds_pending = true;
    } else if (msg_type == TOPO_SET_ROLE && len >= (int)sizeof(SetRoleMessage)) {
        memcpy(&_set_role_msg, data, sizeof(SetRoleMessage));
        _set_role_pending = true;
    } else if (msg_type == TOPO_SET_TINT && len >= (int)sizeof(SetTintMessage)) {
        memcpy(&_set_tint_msg, data, sizeof(SetTintMessage));
        _set_tint_pending = true;
    } else if (msg_type == TOPO_STATE_SYNC && len >= 15) {
        // Queen state broadcast — update g_tod + weather on satellite
        if (!_accept_state_sync) return;
        const StateSyncMessage* ss = reinterpret_cast<const StateSyncMessage*>(data);
        g_tod.night_factor  = ss->night_factor;
        g_tod.day_progress  = ss->day_progress;
        g_tod.phase         = static_cast<DayPhase>(ss->phase);
        g_tod.local_hour    = ss->local_hour;
        g_tod.local_minute  = ss->local_minute;
        // Weather fields (added later — check length for backwards compat)
        if (len >= (int)sizeof(StateSyncMessage)) {
            g_weather.condition     = static_cast<WeatherCondition>(ss->weather);
            g_weather.temperature_c = ss->temperature_c;
            g_weather.valid         = true;
        }
        _state_sync_last_ms = millis();
    } else if (msg_type == TOPO_POP_SYNC
               && len >= (int)offsetof(PopSyncMessage, gatherers)) {
        // Population sync — store latest per sender
        const PopSyncMessage* ps = reinterpret_cast<const PopSyncMessage*>(data);
        for (int f = 0; f < FACE_COUNT; f++) {
            if (_faces[f].link == LINK_CONNECTED && _faces[f].neighbour_id == ps->sender_id) {
                _remote_pop[f] = ps->population;
                _remote_pop_ms[f] = millis();
                // Gatherers/role/tint fields (added later — length-gated)
                _remote_gatherers[f] = (len >= (int)offsetof(PopSyncMessage, role))
                                     ? ps->gatherers : 0;
                _remote_role[f] = (len >= (int)offsetof(PopSyncMessage, tint_r))
                                ? ps->role : 0;
                _remote_tint[f] = (len >= (int)sizeof(PopSyncMessage))
                                ? (((uint32_t)ps->tint_r << 16)
                                   | ((uint32_t)ps->tint_g << 8) | ps->tint_b)
                                : 0;
                break;
            }
        }
    } else if (msg_type == TOPO_DEATH_SYNC && len >= (int)sizeof(DeathSyncMessage)) {
        // Death sync — buffer for coordinator (queen)
        if (_ds_count < DS_BUF_SIZE) {
            int idx = _ds_write;
            memcpy(_ds_buf[idx].data, data, len);
            _ds_buf[idx].len = len;
            _ds_write = (idx + 1) % DS_BUF_SIZE;
            _ds_count++;
        }
    } else if (msg_type == TOPO_GATHER_SYNC && len >= (int)sizeof(GatherSyncMessage)) {
        // Only honour gathers from a face-connected neighbour — with multiple
        // colonies on one LAN/channel, a stranger's broadcast must not move
        // our conkers
        const GatherSyncMessage* gs = reinterpret_cast<const GatherSyncMessage*>(data);
        for (int f = 0; f < FACE_COUNT; f++) {
            if (_faces[f].link == LINK_CONNECTED
                    && _faces[f].neighbour_id == gs->sender_id) {
                memcpy(&_gather_msg, data, sizeof(GatherSyncMessage));
                _gather_pending = true;
                break;
            }
        }
    } else if (msg_type == TOPO_OTA_ANNOUNCE && len >= (int)sizeof(OtaAnnounceMessage)) {
        const OtaAnnounceMessage* oa = reinterpret_cast<const OtaAnnounceMessage*>(data);
        _ota_announce_version = oa->fw_version;
        _ota_announce_pending = true;
    } else if (msg_type == TOPO_OTA_READY && len >= (int)sizeof(OtaReadyMessage)) {
        memcpy(&_ota_ready_msg, data, sizeof(OtaReadyMessage));
        _ota_ready_pending = true;
    } else {
        // Topology control message
        if (len != sizeof(TopologyMessage)) return;
        if (_rx_count >= RX_BUF_SIZE) return;
        int idx = _rx_write;
        memcpy(&_rx_msgs[idx], data, sizeof(TopologyMessage));
        memcpy(_rx_macs[idx], info->src_addr, 6);
        _rx_write = (idx + 1) % RX_BUF_SIZE;
        _rx_count++;
    }
}

// ---------------------------------------------------------------------------
//  Message routing — match incoming message to a face
// ---------------------------------------------------------------------------

static int _find_face_for_msg(const TopologyMessage& msg) {
    // 1. If a face is CONNECTED to this sender, route there
    for (int f = 0; f < FACE_COUNT; f++) {
        if (_faces[f].link == LINK_CONNECTED && _faces[f].neighbour_id == msg.sender_id)
            return f;
    }
    // 2. If a face is in DETECTED_LOCAL (waiting for reply), route there
    for (int f = 0; f < FACE_COUNT; f++) {
        if (_faces[f].link == LINK_DETECTED_LOCAL)
            return f;
    }
    // 3. For unsolicited HELLO: use opposite of sender's face
    if (msg.type == TOPO_HELLO && msg.face < FACE_COUNT) {
        Face remote_face = static_cast<Face>(msg.face);
        Face local_face  = Cfg::FACE_OPPOSITE[remote_face];
        if (_faces[local_face].link == LINK_IDLE || _faces[local_face].link == LINK_ERROR)
            return local_face;
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  Per-face state machine tick
// ---------------------------------------------------------------------------

static void _tick_face(Face f, const TopologyMessage* msg, const uint8_t* msg_mac) {
    FaceState& fs = _faces[f];
    uint32_t now = millis();
    bool detect_fell = (fs.detect_low && !fs.prev_detect_low);
    bool detect_rose = (!fs.detect_low && fs.prev_detect_low);

    switch (fs.link) {
    case LINK_IDLE:
        if (detect_fell) {
            fs.hello_retries = 0;
            fs.hello_sent_ms = now;
            _send(BROADCAST, TOPO_HELLO, f);
            fs.link = LINK_DETECTED_LOCAL;
            Serial.printf("[topo] face %s DETECT LOW, sending HELLO\r\n", FACE_NAMES[f]);
        }
        // Self-heal: DETECT is LOW but no falling edge (e.g. after NTP/OTA
        // channel disruption, or boot while already physically connected).
        // Re-attempt handshake every 5 seconds.
        if (fs.detect_low && !detect_fell && now - fs.hello_sent_ms > 5000) {
            fs.hello_retries = 0;
            fs.hello_sent_ms = now;
            _send(BROADCAST, TOPO_HELLO, f);
            fs.link = LINK_DETECTED_LOCAL;
            Serial.printf("[topo] face %s re-handshake (IDLE+DETECT)\r\n", FACE_NAMES[f]);
        }
        if (msg && msg->type == TOPO_HELLO && msg->sender_id != _my_id) {
            _ensure_peer(fs, msg_mac);  // Register peer BEFORE sending REPLY
            _send(msg_mac, TOPO_REPLY, f);
            _connect_face(f, msg->sender_id, msg_mac);
        }
        break;

    case LINK_DETECTED_LOCAL:
        if (msg && msg->sender_id != _my_id) {
            if (msg->type == TOPO_REPLY || msg->type == TOPO_HELLO) {
                _ensure_peer(fs, msg_mac);  // Register peer BEFORE sending REPLY
                if (msg->type == TOPO_HELLO) _send(msg_mac, TOPO_REPLY, f);
                _connect_face(f, msg->sender_id, msg_mac);
                break;
            }
        }
        if (detect_rose) {
            _disconnect_face(f, "DETECT HIGH before handshake");
            break;
        }
        if (now - fs.hello_sent_ms > HELLO_TIMEOUT_MS) {
            fs.hello_retries++;
            if (fs.hello_retries >= HELLO_MAX_RETRIES) {
                fs.link = LINK_ERROR;
                _hello_timeouts_total++;
                Serial.printf("[topo] face %s HELLO timeout\r\n", FACE_NAMES[f]);
            } else {
                fs.hello_sent_ms = now;
                _send(BROADCAST, TOPO_HELLO, f);
            }
        }
        break;

    case LINK_CONNECTED:
        if (detect_rose) {
            if (fs.peer_added) _send(fs.neighbour_mac, TOPO_GOODBYE, f);
            _disconnect_face(f, "DETECT HIGH");
            break;
        }
        if (msg && msg->sender_id == fs.neighbour_id) {
            if (msg->type == TOPO_GOODBYE) {
                _disconnect_face(f, "GOODBYE received");
                break;
            }
            if (msg->type == TOPO_HEARTBEAT) {
                fs.last_hb_rx_ms = now;
                _neighbours[f].last_seen_ms = now;
            }
            if (msg->type == TOPO_HELLO) {
                _send(msg_mac, TOPO_REPLY, f);
            }
        }
        // Send heartbeat
        if (fs.link == LINK_CONNECTED && now - fs.last_hb_tx_ms > HEARTBEAT_TX_MS) {
            if (fs.peer_added) _send(fs.neighbour_mac, TOPO_HEARTBEAT, f);
            fs.last_hb_tx_ms = now;
        }
        // Heartbeat timeout (suppressed during WiFi windows)
        if (fs.link == LINK_CONNECTED && !_wifi_active &&
            now - fs.last_hb_rx_ms > HEARTBEAT_RX_TIMEOUT_MS) {
            _hb_timeouts_total++;
            _disconnect_face(f, "heartbeat timeout");
        }
        break;

    case LINK_ERROR:
        if (detect_fell || detect_rose) {
            _disconnect_face(f, "error reset");
            break;
        }
        // Rescue: if the other side sends a HELLO while we're stuck, connect immediately
        if (msg && msg->type == TOPO_HELLO && msg->sender_id != _my_id) {
            Serial.printf("[topo] face %s rescued from ERROR by HELLO\r\n", FACE_NAMES[f]);
            _ensure_peer(fs, msg_mac);  // Register peer BEFORE sending REPLY
            _send(msg_mac, TOPO_REPLY, f);
            _connect_face(f, msg->sender_id, msg_mac);
            break;
        }
        // Self-heal: if DETECT still LOW, retry after 5s instead of staying stuck
        if (fs.detect_low && now - fs.hello_sent_ms > 5000) {
            _reinit_fail_count++;
            Serial.printf("[topo] face %s recovering from ERROR (DETECT still LOW) [fail %lu/%lu]\r\n",
                FACE_NAMES[f], (unsigned long)_reinit_fail_count, (unsigned long)REINIT_THRESHOLD);
            fs.link = LINK_IDLE;
            fs.hello_retries = 0;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//  ESP-NOW reinit — recover from dead radio (WiFi.mode(WIFI_OFF), channel drift)
// ---------------------------------------------------------------------------

static bool _set_channel_verified(uint8_t ch) {
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        Serial.printf("[topo] esp_wifi_set_channel(%d) FAILED err=0x%x\r\n", ch, err);
        return false;
    }
    // Verify it actually took
    uint8_t actual = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&actual, &second);
    if (actual != ch) {
        Serial.printf("[topo] channel set to %d but read back %d!\r\n", ch, actual);
        return false;
    }
    return true;
}

static void _espnow_reinit() {
    _reinit_count_total++;

    // Channel scanning: if we've reinit'd multiple times without connecting,
    // the queen may be on a different channel. Try scanning 1-13.
    if (_reinit_count_total >= 2 && !WiFi.isConnected()) {
        _scan_channel++;
        if (_scan_channel > 13) _scan_channel = 1;
        _current_channel = _scan_channel;
        Serial.printf("[topo] ESP-NOW reinit — scanning channel %d\r\n", _current_channel);
    } else {
        Serial.println("[topo] ESP-NOW reinit — radio may be dead");
    }

    // Tear down
    esp_now_deinit();

    if (WiFi.isConnected()) {
        // Queen with persistent WiFi — don't touch channel
        // Channel is locked by STA association
        uint8_t primary;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        _current_channel = primary;
    } else {
        // Satellite (or queen without WiFi):
        // Must clear any stored AP connection state. WiFi.begin() from NTP at boot
        // leaves residual state that causes esp_wifi_set_channel() to fail silently
        // or race with auto-reconnect.
        esp_wifi_disconnect();   // Clear pending connection (safe, doesn't tear down interface)
        delay(10);               // Let WiFi task process
        if (!_set_channel_verified(_current_channel)) {
            // If channel set failed, try harder: full mode reset
            WiFi.mode(WIFI_STA);
            esp_wifi_disconnect();
            delay(10);
            _set_channel_verified(_current_channel);
        }
    }

    // Re-init ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[topo] ESP-NOW reinit FAILED");
        return;
    }
    esp_now_register_recv_cb(_on_recv);

    // Re-add broadcast peer
    esp_now_peer_info_t bp = {};
    memcpy(bp.peer_addr, BROADCAST, 6);
    bp.channel = 0;
    bp.encrypt = false;
    esp_now_add_peer(&bp);

    // Clear face peers (force re-add on next connection)
    for (int f = 0; f < FACE_COUNT; f++) {
        _faces[f].peer_added = false;
    }

    _reinit_fail_count = 0;
    _send_fail_count = 0;

    // Re-read DETECT pins
    for (int f = 0; f < FACE_COUNT; f++) {
        _faces[f].detect_low = (digitalRead(DETECT_PINS[f]) == LOW);
        _faces[f].prev_detect_low = _faces[f].detect_low;
    }

    Serial.println("[topo] ESP-NOW reinit OK");
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void topology_init(TopologyCallback cb) {
    _callback = cb;

    // Configure DETECT pins
    for (int f = 0; f < FACE_COUNT; f++) {
        pinMode(DETECT_PINS[f], INPUT_PULLUP);
        _faces[f] = FaceState{};
        _neighbours[f] = Neighbour{};
    }

    // WiFi STA mode for ESP-NOW
    // If WiFi is already connected (queen staying on AP), use AP's channel.
    // Otherwise, clear any stale STA state and set channel 1.
    if (WiFi.isConnected()) {
        uint8_t primary;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        _current_channel = primary;
        Serial.printf("[topo] WiFi connected, using AP channel %d\r\n", _current_channel);
    } else {
        WiFi.mode(WIFI_STA);
        esp_wifi_disconnect();  // Clear any stored AP from failed NTP at boot
        delay(10);
        // Satellite: start on the channel learned from the boot-time NTP
        // connect (the queen sits on the AP's channel). Falling back to 1
        // strands HELLOs until the slow channel-scan rescue kicks in.
        uint8_t boot_ch = tod_last_wifi_channel();
        _current_channel = (boot_ch >= 1 && boot_ch <= 13) ? boot_ch : 1;
        if (boot_ch)
            Serial.printf("[topo] starting on last WiFi channel %d\r\n", _current_channel);
        _set_channel_verified(_current_channel);
    }

    // Read MAC / derive ID
    esp_read_mac(_my_mac, ESP_MAC_WIFI_STA);
    _my_id = ((uint16_t)_my_mac[4] << 8) | _my_mac[5];

    Serial.printf("[topo] MAC: %02X:%02X:%02X:%02X:%02X:%02X  ID: 0x%04X\r\n",
        _my_mac[0], _my_mac[1], _my_mac[2], _my_mac[3], _my_mac[4], _my_mac[5], _my_id);
    Serial.printf("[topo] DETECT pins: N=%d S=%d W=%d E=%d\r\n",
        DETECT_PINS[FACE_N], DETECT_PINS[FACE_S], DETECT_PINS[FACE_W], DETECT_PINS[FACE_E]);

    // ESP-NOW init
    if (esp_now_init() != ESP_OK) {
        Serial.println("[topo] ESP-NOW init FAILED");
        return;
    }
    esp_now_register_recv_cb(_on_recv);

    // Broadcast peer
    esp_now_peer_info_t bp = {};
    memcpy(bp.peer_addr, BROADCAST, 6);
    bp.channel = 0;
    bp.encrypt = false;
    esp_now_add_peer(&bp);

    // Read initial pin states
    for (int f = 0; f < FACE_COUNT; f++) {
        _faces[f].detect_low = (digitalRead(DETECT_PINS[f]) == LOW);
        _faces[f].prev_detect_low = _faces[f].detect_low;
    }

    _last_any_connect_ms = millis();
    Serial.printf("[topo] ready (boot +%lums)\r\n", (unsigned long)millis());
}

void topology_poll() {
    uint32_t now = millis();
    if (now - _last_poll_ms < POLL_MS) return;
    _last_poll_ms = now;

    // Read DETECT pins
    for (int f = 0; f < FACE_COUNT; f++) {
        _faces[f].prev_detect_low = _faces[f].detect_low;
        _faces[f].detect_low = (digitalRead(DETECT_PINS[f]) == LOW);
    }

    // Drain receive buffer
    while (_rx_count > 0) {
        int read_idx = (_rx_write - _rx_count + RX_BUF_SIZE) % RX_BUF_SIZE;
        TopologyMessage msg;
        uint8_t mac[6];
        memcpy(&msg, &_rx_msgs[read_idx], sizeof(msg));
        memcpy(mac, _rx_macs[read_idx], 6);
        _rx_count--;

        if (msg.sender_id == _my_id) continue;  // ignore own broadcasts

        // Channel follow: if message carries a valid channel different from ours, switch
        if (msg.channel > 0 && msg.channel != _current_channel && !WiFi.isConnected()) {
            Serial.printf("[topo] channel follow: %d -> %d (from 0x%04X)\r\n",
                          _current_channel, msg.channel, msg.sender_id);
            _current_channel = msg.channel;
            esp_wifi_disconnect();  // Ensure clean state before channel change
            _set_channel_verified(_current_channel);
        }

        int target = _find_face_for_msg(msg);
        if (target >= 0) {
            _tick_face(static_cast<Face>(target), &msg, mac);
        }
    }

    // Tick each face (for timeouts, detect edges, heartbeat tx)
    for (int f = 0; f < FACE_COUNT; f++) {
        _tick_face(static_cast<Face>(f), nullptr, nullptr);
    }

    // ESP-NOW reinit — multiple trigger conditions
    bool need_reinit = false;

    // Trigger 1: error recovery counter (normal path)
    if (_reinit_fail_count >= REINIT_THRESHOLD)
        need_reinit = true;

    // Trigger 2: send failures not recovered within 30s
    //            (catches WiFi.mode(WIFI_OFF) where detect pins may read wrong)
    if (_send_fail_count > 0 && now - _first_send_fail_ms > 30000)
        need_reinit = true;

    // Trigger 3: DETECT LOW on any face but no connection for 60s
    bool any_detect_low = false;
    bool any_connected = false;
    for (int f = 0; f < FACE_COUNT; f++) {
        if (_faces[f].detect_low) any_detect_low = true;
        if (_faces[f].link == LINK_CONNECTED) any_connected = true;
    }
    if (any_detect_low && !any_connected && now - _last_any_connect_ms > 60000)
        need_reinit = true;

    if (need_reinit) {
        _espnow_reinit();
        _last_any_connect_ms = now;  // prevent rapid re-triggers
    }
}

uint16_t topology_my_id() { return _my_id; }
const FaceState& topology_face(Face f) { return _faces[f]; }
const Neighbour& topology_neighbour(Face f) { return _neighbours[f]; }
uint8_t  topology_current_channel() { return _current_channel; }

void topology_set_wifi_channel(uint8_t channel) {
    if (channel == 0 || channel == _current_channel) return;
    Serial.printf("[topo] WiFi channel changed: %d -> %d\r\n", _current_channel, channel);
    _current_channel = channel;
    // Don't set channel explicitly — WiFi STA connection handles it
    // ESP-NOW automatically uses the STA channel when connected
}

bool topology_send_to_face(Face f, const uint8_t* data, int len) {
    const FaceState& fs = _faces[f];
    if (fs.link != LINK_CONNECTED || !fs.peer_added) return false;
    esp_err_t err = esp_now_send(fs.neighbour_mac, data, len);
    return err == ESP_OK;
}

int topology_drain_handoffs(PendingHandoff* out, int max_out) {
    int n = 0;
    while (_ho_count > 0 && n < max_out) {
        int idx = (_ho_write - _ho_count + HO_BUF_SIZE) % HO_BUF_SIZE;
        out[n] = _ho_buf[idx];
        _ho_count--;
        n++;
    }
    return n;
}

int topology_drain_handoff_acks(PendingAck* out, int max_out) {
    int n = 0;
    while (_ack_count > 0 && n < max_out) {
        int idx = (_ack_write - _ack_count + ACK_BUF_SIZE) % ACK_BUF_SIZE;
        out[n] = _ack_buf[idx];
        _ack_count--;
        n++;
    }
    return n;
}

int topology_drain_death_syncs(PendingDeathSync* out, int max_out) {
    int n = 0;
    while (_ds_count > 0 && n < max_out) {
        int idx = (_ds_write - _ds_count + DS_BUF_SIZE) % DS_BUF_SIZE;
        out[n] = _ds_buf[idx];
        _ds_count--;
        n++;
    }
    return n;
}

bool topology_has_gather(GatherSyncMessage* out) {
    if (!_gather_pending) return false;
    _gather_pending = false;
    *out = _gather_msg;
    return true;
}

uint16_t topology_remote_population(Face f) {
    return _remote_pop[f];
}

bool topology_pop_sync_fresh(Face f) {
    uint32_t t = _remote_pop_ms[f];
    return t != 0 && (millis() - t) < 15000;
}

uint16_t topology_remote_gatherers(Face f) {
    return _remote_gatherers[f];
}

uint8_t topology_remote_role(Face f) {
    return _remote_role[f];
}

bool topology_has_set_role(SetRoleMessage* out) {
    if (!_set_role_pending) return false;
    memcpy(out, (const void*)&_set_role_msg, sizeof(SetRoleMessage));
    _set_role_pending = false;
    return true;
}

uint32_t topology_remote_tint(Face f) {
    return _remote_tint[f];
}

bool topology_has_set_tint(SetTintMessage* out) {
    if (!_set_tint_pending) return false;
    memcpy(out, (const void*)&_set_tint_msg, sizeof(SetTintMessage));
    _set_tint_pending = false;
    return true;
}

void topology_set_accept_state_sync(bool accept) {
    _accept_state_sync = accept;
}

const BoundaryPheroData& topology_boundary_phero(Face f) {
    return _boundary_phero[f];
}

bool topology_has_announce(AnnounceMessage* out) {
    if (!_announce_pending) return false;
    memcpy(out, &_announce_msg, sizeof(AnnounceMessage));
    _announce_pending = false;
    return true;
}

bool topology_has_wifi_creds(WifiCredsMessage* out) {
    if (!_wifi_creds_pending) return false;
    memcpy(out, &_wifi_creds_msg, sizeof(WifiCredsMessage));
    _wifi_creds_pending = false;
    return true;
}

bool topology_has_state_sync() {
    if (_state_sync_last_ms == 0) return false;
    return (millis() - _state_sync_last_ms) < 30000;
}

uint32_t topology_state_sync_age_ms() {
    if (_state_sync_last_ms == 0) return UINT32_MAX;
    return millis() - _state_sync_last_ms;
}

bool topology_broadcast(const uint8_t* data, int len) {
    return esp_now_send(BROADCAST, data, len) == ESP_OK;
}

bool topology_ota_check(uint32_t* fw_version) {
    if (!_ota_announce_pending) return false;
    *fw_version = _ota_announce_version;
    _ota_announce_pending = false;
    return true;
}

bool topology_ota_server(OtaReadyMessage* out) {
    if (!_ota_ready_pending) return false;
    memcpy(out, &_ota_ready_msg, sizeof(OtaReadyMessage));
    _ota_ready_pending = false;
    return true;
}

void topology_set_wifi_active(bool active) {
    _wifi_active = active;
    if (!active) {
        // WiFi done — reset heartbeat timestamps so faces don't immediately timeout
        uint32_t now = millis();
        for (int f = 0; f < FACE_COUNT; f++) {
            if (_faces[f].link == LINK_CONNECTED)
                _faces[f].last_hb_rx_ms = now;
        }
    }
}

// ---------------------------------------------------------------------------
//  Debug overlay
// ---------------------------------------------------------------------------

void topology_draw_overlay(void* canvas_ptr) {
    Arduino_Canvas* gfx = static_cast<Arduino_Canvas*>(canvas_ptr);

    // Semi-transparent dark background
    // Draw a filled rect with alpha (approximated by dark color)
    int ox = 40, oy = 60, w = 400, h = 200;
    gfx->fillRect(ox, oy, w, h, gfx->color565(10, 10, 10));
    gfx->drawRect(ox, oy, w, h, gfx->color565(80, 80, 80));

    gfx->setTextSize(2);
    gfx->setTextWrap(false);

    // Title
    gfx->setCursor(ox + 10, oy + 8);
    gfx->setTextColor(gfx->color565(200, 200, 200));
    char buf[64];
    snprintf(buf, sizeof(buf), "Topology  ID:0x%04X", _my_id);
    gfx->print(buf);

    // Per-face rows
    int ry = oy + 36;
    for (int f = 0; f < FACE_COUNT; f++) {
        const FaceState& fs = _faces[f];
        const Neighbour& nb = _neighbours[f];

        uint16_t color;
        if (nb.present)
            color = gfx->color565(80, 255, 80);     // green
        else if (fs.link == LINK_ERROR)
            color = gfx->color565(255, 80, 80);     // red
        else if (fs.detect_low)
            color = gfx->color565(255, 255, 80);    // yellow
        else
            color = gfx->color565(120, 120, 120);   // grey

        gfx->setCursor(ox + 10, ry);
        gfx->setTextColor(color);

        if (nb.present) {
            snprintf(buf, sizeof(buf), "%s: %s  0x%04X  pin=%s",
                FACE_NAMES[f], link_str(fs.link), nb.module_id,
                fs.detect_low ? "LOW" : "HIGH");
        } else {
            snprintf(buf, sizeof(buf), "%s: %s  pin=%s",
                FACE_NAMES[f], link_str(fs.link),
                fs.detect_low ? "LOW" : "HIGH");
        }
        gfx->print(buf);
        ry += 28;
    }

    // Uptime
    gfx->setCursor(ox + 10, ry + 8);
    gfx->setTextColor(gfx->color565(120, 120, 120));
    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "up %lum%lus", (unsigned long)(up / 60), (unsigned long)(up % 60));
    gfx->print(buf);
}

void topology_status() {
    uint8_t actual_ch = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&actual_ch, &second);

    Serial.println("[topo] --- status ---");
    Serial.printf("  my_id:       0x%04X\r\n", _my_id);
    Serial.printf("  channel:     %d (actual radio: %d)\r\n", _current_channel, actual_ch);
    Serial.printf("  wifi:        %s\r\n", WiFi.isConnected() ? "connected" : "disconnected");
    Serial.printf("  reinit_tot:  %d  scan_ch: %d\r\n", _reinit_count_total, _scan_channel);
    Serial.printf("  send_fails:  %lu  reinit_fails: %lu/%lu\r\n",
        (unsigned long)_send_fail_count, (unsigned long)_reinit_fail_count, (unsigned long)REINIT_THRESHOLD);
    Serial.printf("  hello_to:    %lu  hb_to: %lu (lifetime)\r\n",
        (unsigned long)_hello_timeouts_total, (unsigned long)_hb_timeouts_total);

    const char* link_names[] = {"IDLE", "DETECTED", "CONNECTED", "ERROR"};
    for (int f = 0; f < FACE_COUNT; f++) {
        const FaceState& fs = _faces[f];
        Serial.printf("  face %s: %s", FACE_NAMES[f], link_names[fs.link]);
        if (fs.link == LINK_CONNECTED) {
            uint32_t hb_age = (millis() - fs.last_hb_rx_ms) / 1000;
            Serial.printf("  id=0x%04X  peer=%s  hb_rx=%lus ago  mac=%02X:%02X:%02X:%02X:%02X:%02X",
                fs.neighbour_id, fs.peer_added ? "yes" : "NO",
                (unsigned long)hb_age,
                fs.neighbour_mac[0], fs.neighbour_mac[1], fs.neighbour_mac[2],
                fs.neighbour_mac[3], fs.neighbour_mac[4], fs.neighbour_mac[5]);
        }
        Serial.printf("  detect=%s\r\n", fs.detect_low ? "LOW" : "HIGH");
    }
    Serial.println("[topo] -----------------");
}
