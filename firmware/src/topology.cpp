/* Topology detection — per-face DETECT + ESP-NOW handshake. */
#include "topology.h"
#include "pin_config.h"
#include "time_of_day.h"
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

// Receive ring buffer (ISR -> main loop) — topology messages
static const int RX_BUF_SIZE = 8;
static volatile int _rx_write = 0;
static volatile int _rx_count = 0;
static TopologyMessage _rx_msgs[RX_BUF_SIZE];
static uint8_t         _rx_macs[RX_BUF_SIZE][6];

// Handoff receive buffer (ISR -> coordinator via drain)
static const int HO_BUF_SIZE = 4;
static volatile int _ho_write = 0;
static volatile int _ho_count = 0;
static PendingHandoff _ho_buf[HO_BUF_SIZE];

// Remote population per face (updated by TOPO_POP_SYNC messages)
static volatile uint16_t _remote_pop[FACE_COUNT] = {0, 0, 0, 0};

// Boundary pheromone data from neighbours
static BoundaryPheroData _boundary_phero[FACE_COUNT] = {};

// Queen state sync
static volatile uint32_t _state_sync_last_ms = 0;

// Chamber announcement (single-shot, cleared after read)
static volatile bool _announce_pending = false;
static AnnounceMessage _announce_msg;

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
    msg.reserved  = 0;
    esp_now_send(mac, (const uint8_t*)&msg, sizeof(msg));
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

    Serial.printf("[topo] face %s connected to module 0x%04X\n", FACE_NAMES[f], id);
    if (_callback) _callback(f, true, id);
}

static void _disconnect_face(Face f, const char* reason) {
    FaceState& fs = _faces[f];
    uint16_t old_id = fs.neighbour_id;
    fs.link         = LINK_IDLE;
    fs.neighbour_id = 0;
    fs.hello_retries = 0;

    _neighbours[f].present = false;
    _neighbours[f].module_id = 0;
    _remote_pop[f] = 0;
    memset(&_boundary_phero[f], 0, sizeof(BoundaryPheroData));

    Serial.printf("[topo] face %s disconnected (%s)\n", FACE_NAMES[f], reason);
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
        if (_ho_count >= HO_BUF_SIZE || len > 250) return;
        int idx = _ho_write;
        memcpy(_ho_buf[idx].data, data, len);
        _ho_buf[idx].len = len;
        _ho_write = (idx + 1) % HO_BUF_SIZE;
        _ho_count++;
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
    } else if (msg_type == TOPO_STATE_SYNC && len >= (int)sizeof(StateSyncMessage)) {
        // Queen state broadcast — update g_tod on satellite
        const StateSyncMessage* ss = reinterpret_cast<const StateSyncMessage*>(data);
        g_tod.night_factor  = ss->night_factor;
        g_tod.day_progress  = ss->day_progress;
        g_tod.phase         = static_cast<DayPhase>(ss->phase);
        g_tod.local_hour    = ss->local_hour;
        g_tod.local_minute  = ss->local_minute;
        _state_sync_last_ms = millis();
    } else if (msg_type == TOPO_POP_SYNC && len >= (int)sizeof(PopSyncMessage)) {
        // Population sync — store latest per sender
        const PopSyncMessage* ps = reinterpret_cast<const PopSyncMessage*>(data);
        for (int f = 0; f < FACE_COUNT; f++) {
            if (_faces[f].link == LINK_CONNECTED && _faces[f].neighbour_id == ps->sender_id) {
                _remote_pop[f] = ps->population;
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
        if (_faces[local_face].link == LINK_IDLE)
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
            Serial.printf("[topo] face %s DETECT LOW, sending HELLO\n", FACE_NAMES[f]);
        }
        // Self-heal: DETECT is LOW but no falling edge (e.g. after NTP/OTA
        // channel disruption, or boot while already physically connected).
        // Re-attempt handshake every 5 seconds.
        if (fs.detect_low && !detect_fell && now - fs.hello_sent_ms > 5000) {
            fs.hello_retries = 0;
            fs.hello_sent_ms = now;
            _send(BROADCAST, TOPO_HELLO, f);
            fs.link = LINK_DETECTED_LOCAL;
            Serial.printf("[topo] face %s re-handshake (IDLE+DETECT)\n", FACE_NAMES[f]);
        }
        if (msg && msg->type == TOPO_HELLO && msg->sender_id != _my_id) {
            _send(msg_mac, TOPO_REPLY, f);
            _connect_face(f, msg->sender_id, msg_mac);
        }
        break;

    case LINK_DETECTED_LOCAL:
        if (msg && msg->sender_id != _my_id) {
            if (msg->type == TOPO_REPLY || msg->type == TOPO_HELLO) {
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
                Serial.printf("[topo] face %s HELLO timeout\n", FACE_NAMES[f]);
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
        // Heartbeat timeout
        if (fs.link == LINK_CONNECTED && now - fs.last_hb_rx_ms > HEARTBEAT_RX_TIMEOUT_MS) {
            _disconnect_face(f, "heartbeat timeout");
        }
        break;

    case LINK_ERROR:
        if (detect_fell || detect_rose) {
            _disconnect_face(f, "error reset");
        }
        break;
    }
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

    // WiFi STA mode for ESP-NOW (NTP may have turned WiFi off)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Force channel 1 — after NTP, the radio may be on the AP's channel.
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    // Read MAC / derive ID
    esp_read_mac(_my_mac, ESP_MAC_WIFI_STA);
    _my_id = ((uint16_t)_my_mac[4] << 8) | _my_mac[5];

    Serial.printf("[topo] MAC: %02X:%02X:%02X:%02X:%02X:%02X  ID: 0x%04X\n",
        _my_mac[0], _my_mac[1], _my_mac[2], _my_mac[3], _my_mac[4], _my_mac[5], _my_id);
    Serial.printf("[topo] DETECT pins: N=%d S=%d W=%d E=%d\n",
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

    Serial.println("[topo] ready");
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

        int target = _find_face_for_msg(msg);
        if (target >= 0) {
            _tick_face(static_cast<Face>(target), &msg, mac);
        }
    }

    // Tick each face (for timeouts, detect edges, heartbeat tx)
    for (int f = 0; f < FACE_COUNT; f++) {
        _tick_face(static_cast<Face>(f), nullptr, nullptr);
    }
}

uint16_t topology_my_id() { return _my_id; }
const FaceState& topology_face(Face f) { return _faces[f]; }
const Neighbour& topology_neighbour(Face f) { return _neighbours[f]; }

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

uint16_t topology_remote_population(Face f) {
    return _remote_pop[f];
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
