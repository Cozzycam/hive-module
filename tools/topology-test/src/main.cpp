// Topology Detection Bench Prototype
// Standalone bench tool for Waveshare ESP32-S3-Touch-LCD-3.5B
// Tests per-face DETECT pin + ESP-NOW handshake for inter-module topology.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <Arduino_GFX_Library.h>

// ---------------------------------------------------------------------------
//  Pin config
// ---------------------------------------------------------------------------

// Production DETECT pin allocation (4 faces, confirmed free on J8 header):
//   DETECT_N  = GPIO 17   (North)
//   DETECT_E  = GPIO 18   (East)
//   DETECT_S  = GPIO 21   (South)
//   DETECT_W  = GPIO 38   (West)
// All support pull-up, none are strapping pins, none collide with
// LCD QSPI (1-5,12), backlight (6), I2C (7,8), SD_MMC (9-11),
// I2S audio (13-16), USB (19-20), or UART (43-44).
//
// This bench test uses a single face (East) to validate the protocol.
#define DETECT_PIN  18

// Display (same as charge-diagnostic / main firmware)
#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK   5
#define LCD_QSPI_D0    1
#define LCD_QSPI_D1    2
#define LCD_QSPI_D2    3
#define LCD_QSPI_D3    4
#define LCD_BL          6
#define LCD_WIDTH     320
#define LCD_HEIGHT    480

// I2C (for TCA9554 LCD reset only)
#define I2C_SDA         8
#define I2C_SCL         7
#define I2C_FREQ   400000

// TCA9554 I/O expander
#define TCA9554_ADDR    0x20
#define TCA9554_LCD_RST 1

// ---------------------------------------------------------------------------
//  ESP-NOW protocol
// ---------------------------------------------------------------------------

enum MsgType : uint8_t {
    MSG_HELLO     = 0x01,
    MSG_REPLY     = 0x02,
    MSG_GOODBYE   = 0x03,
    MSG_HEARTBEAT = 0x04,
};

struct __attribute__((packed)) TopologyMessage {
    uint8_t  type;
    uint16_t sender_id;
    uint8_t  face;       // 0=N 1=E 2=S 3=W — always 1 for this test
    uint8_t  reserved;
};

static const char* msg_type_str(uint8_t t) {
    switch (t) {
        case MSG_HELLO:     return "HELLO";
        case MSG_REPLY:     return "REPLY";
        case MSG_GOODBYE:   return "GOODBYE";
        case MSG_HEARTBEAT: return "HEARTBEAT";
        default:            return "???";
    }
}

// ---------------------------------------------------------------------------
//  State machine
// ---------------------------------------------------------------------------

enum State : uint8_t {
    STATE_IDLE,
    STATE_DETECTED_LOCAL,   // we saw DETECT go LOW, broadcasting HELLO
    STATE_DETECTED_REMOTE,  // we received a HELLO while DETECT is HIGH
    STATE_CONNECTED,
    STATE_ERROR,
};

static const char* state_str(State s) {
    switch (s) {
        case STATE_IDLE:            return "IDLE";
        case STATE_DETECTED_LOCAL:  return "DETECTED_LOCAL";
        case STATE_DETECTED_REMOTE: return "DETECTED_REMOTE";
        case STATE_CONNECTED:       return "CONNECTED";
        case STATE_ERROR:           return "ERROR";
        default:                    return "???";
    }
}

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------

// Display
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK,
    LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, -1, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// Identity
static uint16_t my_id = 0;
static uint8_t  my_mac[6];

// State
static State    state           = STATE_IDLE;
static uint16_t neighbour_id    = 0;
static uint8_t  neighbour_mac[6] = {0};
static bool     neighbour_peer_added = false;

// Timing
static uint32_t state_enter_ms  = 0;
static uint32_t last_event_ms   = 0;
static const char* last_event_str = "boot";
static uint32_t last_heartbeat_tx_ms = 0;
static uint32_t last_heartbeat_rx_ms = 0;
static uint8_t  hello_retries   = 0;
static uint32_t hello_sent_ms   = 0;

// DETECT pin
static bool     detect_low      = false;
static bool     prev_detect_low = false;

// ESP-NOW receive buffer (single message, ISR-safe via volatile flag)
static volatile bool     rx_pending = false;
static TopologyMessage   rx_msg;
static uint8_t           rx_mac[6];

// Broadcast address
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

static const uint32_t POLL_INTERVAL_MS      = 50;
static const uint32_t DISPLAY_INTERVAL_MS   = 250;
static const uint32_t HELLO_TIMEOUT_MS      = 1000;
static const uint8_t  HELLO_MAX_RETRIES     = 3;
static const uint32_t HEARTBEAT_TX_MS       = 1000;
static const uint32_t HEARTBEAT_TIMEOUT_MS  = 3000;  // 3 missed beats

// ---------------------------------------------------------------------------
//  TCA9554 / LCD reset (same as charge-diagnostic)
// ---------------------------------------------------------------------------

static void tca9554_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}
static uint8_t tca9554_read_reg(uint8_t reg) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    return Wire.read();
}
static void lcd_reset() {
    uint8_t cfg = tca9554_read_reg(0x03);
    tca9554_write_reg(0x03, cfg & ~(1 << TCA9554_LCD_RST));
    uint8_t out = tca9554_read_reg(0x01);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out & ~(1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(200);
}

// ---------------------------------------------------------------------------
//  ESP-NOW send / receive
// ---------------------------------------------------------------------------

static void send_msg(const uint8_t* dest_mac, uint8_t type) {
    TopologyMessage msg;
    msg.type      = type;
    msg.sender_id = my_id;
    msg.face      = 1;  // placeholder — always East for this bench test
    msg.reserved  = 0;

    esp_err_t err = esp_now_send(dest_mac, (const uint8_t*)&msg, sizeof(msg));
    Serial.printf("[tx] %s -> %02X:%02X:%02X:%02X:%02X:%02X (id=0x%04X) %s\n",
        msg_type_str(type),
        dest_mac[0], dest_mac[1], dest_mac[2],
        dest_mac[3], dest_mac[4], dest_mac[5],
        my_id,
        err == ESP_OK ? "ok" : "FAIL");
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(TopologyMessage)) return;
    if (rx_pending) return;  // drop if main loop hasn't consumed previous

    memcpy(&rx_msg, data, sizeof(TopologyMessage));
    memcpy(rx_mac, info->src_addr, 6);
    rx_pending = true;
}

static void ensure_peer(const uint8_t* mac) {
    if (neighbour_peer_added && memcmp(neighbour_mac, mac, 6) == 0) return;

    // Remove old peer if different
    if (neighbour_peer_added) {
        esp_now_del_peer(neighbour_mac);
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    memcpy(neighbour_mac, mac, 6);
    neighbour_peer_added = true;
}

// ---------------------------------------------------------------------------
//  State transitions
// ---------------------------------------------------------------------------

static void set_state(State new_state, const char* event) {
    if (new_state != state) {
        Serial.printf("[state] %s -> %s (%s)\n", state_str(state), state_str(new_state), event);
    }
    state = new_state;
    state_enter_ms = millis();
    last_event_ms  = millis();
    last_event_str = event;
}

static void go_idle(const char* reason) {
    neighbour_id = 0;
    hello_retries = 0;
    set_state(STATE_IDLE, reason);
}

// ---------------------------------------------------------------------------
//  Main state machine tick
// ---------------------------------------------------------------------------

static void state_machine_tick() {
    uint32_t now = millis();

    // --- Process incoming message if any ---
    TopologyMessage incoming;
    uint8_t incoming_mac[6];
    bool have_msg = false;

    if (rx_pending) {
        memcpy(&incoming, &rx_msg, sizeof(incoming));
        memcpy(incoming_mac, rx_mac, 6);
        rx_pending = false;
        have_msg = true;

        Serial.printf("[rx] %s from 0x%04X (%02X:%02X:%02X:%02X:%02X:%02X)\n",
            msg_type_str(incoming.type), incoming.sender_id,
            incoming_mac[0], incoming_mac[1], incoming_mac[2],
            incoming_mac[3], incoming_mac[4], incoming_mac[5]);
    }

    // --- DETECT pin edge detection ---
    bool detect_fell = (detect_low && !prev_detect_low);   // just went LOW
    bool detect_rose = (!detect_low && prev_detect_low);   // just went HIGH

    // --- State handlers ---
    switch (state) {

    case STATE_IDLE:
        // DETECT went LOW -> initiate handshake
        if (detect_fell) {
            hello_retries = 0;
            hello_sent_ms = now;
            send_msg(BROADCAST_MAC, MSG_HELLO);
            set_state(STATE_DETECTED_LOCAL, "DETECT LOW");
        }
        // Received HELLO from someone while our DETECT is HIGH (single-jumper, we're Board B)
        if (have_msg && incoming.type == MSG_HELLO) {
            ensure_peer(incoming_mac);
            neighbour_id = incoming.sender_id;
            send_msg(incoming_mac, MSG_REPLY);
            last_heartbeat_rx_ms = now;
            last_heartbeat_tx_ms = now;
            set_state(STATE_CONNECTED, "HELLO received");
        }
        break;

    case STATE_DETECTED_LOCAL:
        // Waiting for REPLY to our HELLO
        if (have_msg && incoming.type == MSG_REPLY && incoming.sender_id != my_id) {
            ensure_peer(incoming_mac);
            neighbour_id = incoming.sender_id;
            last_heartbeat_rx_ms = now;
            last_heartbeat_tx_ms = now;
            set_state(STATE_CONNECTED, "REPLY received");
        }
        // Also accept a HELLO (two-jumper case: both boards broadcast simultaneously)
        else if (have_msg && incoming.type == MSG_HELLO && incoming.sender_id != my_id) {
            ensure_peer(incoming_mac);
            neighbour_id = incoming.sender_id;
            send_msg(incoming_mac, MSG_REPLY);
            last_heartbeat_rx_ms = now;
            last_heartbeat_tx_ms = now;
            set_state(STATE_CONNECTED, "mutual HELLO");
        }
        // DETECT went back HIGH before handshake completed
        else if (detect_rose) {
            go_idle("DETECT HIGH before handshake");
        }
        // Timeout -> retry
        else if (now - hello_sent_ms > HELLO_TIMEOUT_MS) {
            hello_retries++;
            if (hello_retries >= HELLO_MAX_RETRIES) {
                set_state(STATE_ERROR, "HELLO timeout (3 retries)");
            } else {
                Serial.printf("[retry] HELLO attempt %d/%d\n", hello_retries + 1, HELLO_MAX_RETRIES);
                hello_sent_ms = now;
                send_msg(BROADCAST_MAC, MSG_HELLO);
            }
        }
        break;

    case STATE_DETECTED_REMOTE:
        // This state is entered when we receive HELLO while DETECT is HIGH.
        // We already replied and moved to CONNECTED in the IDLE handler above,
        // so this is effectively unused. Kept for completeness.
        break;

    case STATE_CONNECTED:
        // DETECT went HIGH -> we disconnected locally
        if (detect_rose) {
            if (neighbour_peer_added) {
                send_msg(neighbour_mac, MSG_GOODBYE);
            }
            go_idle("DETECT HIGH");
        }
        // Received GOODBYE
        else if (have_msg && incoming.type == MSG_GOODBYE && incoming.sender_id == neighbour_id) {
            go_idle("GOODBYE received");
        }
        // Received HEARTBEAT
        else if (have_msg && incoming.type == MSG_HEARTBEAT && incoming.sender_id == neighbour_id) {
            last_heartbeat_rx_ms = now;
        }
        // Received late HELLO from same neighbour (duplicate) — just reply again
        else if (have_msg && incoming.type == MSG_HELLO && incoming.sender_id == neighbour_id) {
            send_msg(incoming_mac, MSG_REPLY);
        }
        // Received late REPLY — accept silently
        else if (have_msg && incoming.type == MSG_REPLY && incoming.sender_id == neighbour_id) {
            // already connected, ignore
        }
        // Send heartbeat
        if (state == STATE_CONNECTED && now - last_heartbeat_tx_ms > HEARTBEAT_TX_MS) {
            if (neighbour_peer_added) {
                send_msg(neighbour_mac, MSG_HEARTBEAT);
            }
            last_heartbeat_tx_ms = now;
        }
        // Heartbeat timeout
        if (state == STATE_CONNECTED && now - last_heartbeat_rx_ms > HEARTBEAT_TIMEOUT_MS) {
            go_idle("heartbeat timeout");
        }
        break;

    case STATE_ERROR:
        // Any DETECT transition resets to IDLE
        if (detect_fell || detect_rose) {
            go_idle("DETECT transition (error reset)");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//  Display
// ---------------------------------------------------------------------------

static void draw_row(int y, const char* label, const char* value) {
    gfx->setCursor(10, y);
    gfx->setTextColor(WHITE);
    gfx->print(label);
    gfx->setCursor(280, y);
    gfx->print(value);
}

static void update_display() {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(3);
    gfx->setTextWrap(false);

    char buf[64];
    int y = 20;
    int rowH = 50;

    // Row 1: My ID
    snprintf(buf, sizeof(buf), "0x%04X", my_id);
    draw_row(y, "My ID:", buf);
    y += rowH;

    // Row 2: DETECT pin
    draw_row(y, "DETECT pin:", detect_low ? "LOW" : "HIGH");
    y += rowH;

    // Row 3: State
    draw_row(y, "State:", state_str(state));
    y += rowH;

    // Row 4: Neighbour
    if (neighbour_id != 0) {
        snprintf(buf, sizeof(buf), "0x%04X", neighbour_id);
    } else {
        snprintf(buf, sizeof(buf), "none");
    }
    draw_row(y, "Neighbour:", buf);
    y += rowH;

    // Row 5: Last event
    uint32_t ago_ms = millis() - last_event_ms;
    snprintf(buf, sizeof(buf), "%lu.%lus - %s",
        (unsigned long)(ago_ms / 1000),
        (unsigned long)((ago_ms % 1000) / 100),
        last_event_str);
    draw_row(y, "Last evt:", buf);
    y += rowH;

    // Row 6: Uptime
    uint32_t up_s = millis() / 1000;
    snprintf(buf, sizeof(buf), "%lum %lus", (unsigned long)(up_s / 60), (unsigned long)(up_s % 60));
    draw_row(y, "Uptime:", buf);

    gfx->flush();
}

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Topology Detection Bench Prototype ===");

    // DETECT pin
    pinMode(DETECT_PIN, INPUT_PULLUP);
    Serial.printf("[init] DETECT_PIN = GPIO %d\n", DETECT_PIN);

    // I2C (for TCA9554 LCD reset)
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // LCD reset via TCA9554
    lcd_reset();

    // Backlight
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // Display init
    gfx->begin();
    gfx->setRotation(1);  // landscape 480x320
    gfx->fillScreen(BLACK);
    gfx->setTextSize(3);
    gfx->setTextColor(WHITE);
    gfx->setCursor(10, 140);
    gfx->print("Initializing WiFi...");
    gfx->flush();

    // WiFi STA mode (required for ESP-NOW)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Get MAC and derive 16-bit ID
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    my_id = ((uint16_t)my_mac[4] << 8) | my_mac[5];

    Serial.printf("[init] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    Serial.printf("[init] My ID: 0x%04X\n", my_id);

    // ESP-NOW init
    if (esp_now_init() != ESP_OK) {
        Serial.println("[init] ESP-NOW init FAILED");
        gfx->fillScreen(BLACK);
        gfx->setTextColor(RED);
        gfx->setCursor(10, 140);
        gfx->print("ESP-NOW INIT FAILED");
        gfx->flush();
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(on_recv);

    // Add broadcast peer
    esp_now_peer_info_t broadcast_peer = {};
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, 6);
    broadcast_peer.channel = 0;
    broadcast_peer.encrypt = false;
    esp_now_add_peer(&broadcast_peer);

    // Init state
    detect_low = (digitalRead(DETECT_PIN) == LOW);
    prev_detect_low = detect_low;
    state_enter_ms = millis();
    last_event_ms  = millis();

    Serial.println("[init] Ready. Waiting for DETECT events...");
    Serial.printf("[init] DETECT pin is currently %s\n", detect_low ? "LOW" : "HIGH");
}

// ---------------------------------------------------------------------------
//  Loop
// ---------------------------------------------------------------------------

static uint32_t last_poll_ms    = 0;
static uint32_t last_display_ms = 0;

void loop() {
    uint32_t now = millis();

    // Poll DETECT pin at 50ms interval
    if (now - last_poll_ms >= POLL_INTERVAL_MS) {
        last_poll_ms = now;
        prev_detect_low = detect_low;
        detect_low = (digitalRead(DETECT_PIN) == LOW);
        state_machine_tick();
    }

    // Update display at 250ms interval
    if (now - last_display_ms >= DISPLAY_INTERVAL_MS) {
        last_display_ms = now;
        update_display();
    }
}
