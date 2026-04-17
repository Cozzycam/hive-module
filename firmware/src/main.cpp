/*
 * Hive Module — Live colony simulation on ESP32-S3.
 *
 * Serial commands (via device monitor):
 *   +/f  -- faster sim speed       -/s  -- slower sim speed
 *   1-7  -- set speed directly     (2,5,8,15,30,60,150 tps)
 *   z    -- zoom in                x    -- zoom out
 *   ?    -- print current status
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <esp_random.h>

#include "pin_config.h"
#include "config.h"
#include "rng.h"
#include "sim.h"
#include "renderer.h"

// -- TCA9554 I/O expander --------------------------------------------

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
    // Set P1 as output
    uint8_t cfg = tca9554_read_reg(0x03);
    tca9554_write_reg(0x03, cfg & ~(1 << TCA9554_LCD_RST));
    // Toggle reset
    uint8_t out = tca9554_read_reg(0x01);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out & ~(1 << TCA9554_LCD_RST));
    delay(10);
    tca9554_write_reg(0x01, out | (1 << TCA9554_LCD_RST));
    delay(200);
}

// -- Display ---------------------------------------------------------

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK,
    LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, -1, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// -- Sim + Renderer --------------------------------------------------

static Sim sim;
static Renderer renderer;

// -- Speed control ---------------------------------------------------

static const int SPEED_LEVELS[] = {2, 5, 8, 15, 30, 60, 150, 300, 600, 1000};
static const int NUM_SPEEDS = sizeof(SPEED_LEVELS) / sizeof(SPEED_LEVELS[0]);
static int speed_index = 2;  // default = 8 tps

static unsigned long tick_interval_ms() {
    return 1000 / SPEED_LEVELS[speed_index];
}

// -- Timing ----------------------------------------------------------

static unsigned long last_tick_ms = 0;
static unsigned long last_frame_ms = 0;

// -- Serial command handling -----------------------------------------

static void print_status() {
    int day = sim.tick_count / Cfg::TICKS_PER_SIM_DAY;
    Serial.printf(
        "Day %d | Pop %d | Food %.0f | E%d L%d P%d | "
        "Pressure %.2f | Speed %d tps\n",
        day, sim.colony.population, sim.colony.food_total,
        sim.colony.brood_egg, sim.colony.brood_larva, sim.colony.brood_pupa,
        sim.colony.food_pressure(),
        SPEED_LEVELS[speed_index]);
}

static void handle_serial() {
    while (Serial.available()) {
        char c = Serial.read();
        switch (c) {
        case '+': case 'f':
            if (speed_index < NUM_SPEEDS - 1) speed_index++;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '-': case 's':
            if (speed_index > 0) speed_index--;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
        case '9':
            speed_index = c - '1';
            if (speed_index >= NUM_SPEEDS) speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case '0':  // 0 = level 10 (1000 tps)
            speed_index = NUM_SPEEDS - 1;
            Serial.printf("Speed: %d tps\n", SPEED_LEVELS[speed_index]);
            break;
        case 'r':  // force full redraw
            renderer.force_full_redraw();
            Serial.println("Full redraw");
            break;
        case '?':
            print_status();
            break;
        default:
            break;
        }
    }
}

// -- Arduino entry points --------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.println("Hive Module -- live sim");
    Serial.println("Commands: +/- speed, 1-9/0 speed level, r redraw, ? status");

    g_rng = Rng(esp_random());

    Wire.begin(I2C_SDA, I2C_SCL);
    lcd_reset();

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    if (!gfx->begin()) {
        Serial.println("ERROR: display init failed");
        while (true) delay(1000);
    }
    gfx->setRotation(1);

    sim.init();
    renderer.init(gfx);

    last_tick_ms = millis();
    last_frame_ms = millis();

    Serial.println("Running.");
}

void loop() {
    unsigned long now = millis();
    unsigned long interval = tick_interval_ms();

    handle_serial();

    // Sim ticks — run multiple if at high speed
    while (now - last_tick_ms >= interval) {
        last_tick_ms += interval;
        sim.tick();

        // Prevent spiral-of-death
        if (now - last_tick_ms > interval * 4) {
            last_tick_ms = now;
            break;
        }
    }

    // Render at ~30fps
    if (now - last_frame_ms >= 33) {
        last_frame_ms = now;

        float lerp_t = static_cast<float>(now - last_tick_ms) / interval;
        if (lerp_t < 0.0f) lerp_t = 0.0f;
        if (lerp_t > 1.0f) lerp_t = 1.0f;

        renderer.draw(sim.chamber, lerp_t);
        renderer.flush();
    }

    // Periodic status
    static unsigned long last_debug = 0;
    if (now - last_debug >= 5000) {
        last_debug = now;
        print_status();
    }
}
