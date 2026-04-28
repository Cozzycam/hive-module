// AXP2101 Charge Diagnostic
// Standalone bench tool for Waveshare ESP32-S3-Touch-LCD-3.5B
// Displays PMIC charge state on screen. Read-only, minimal.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// --- Display: AXS15231B over QSPI ---
#define LCD_QSPI_CS   12
#define LCD_QSPI_CLK   5
#define LCD_QSPI_D0    1
#define LCD_QSPI_D1    2
#define LCD_QSPI_D2    3
#define LCD_QSPI_D3    4
#define LCD_BL          6
#define LCD_WIDTH     320
#define LCD_HEIGHT    480

// TCA9554 I/O expander (handles LCD reset)
#define TCA9554_ADDR    0x20
#define TCA9554_LCD_RST 1  // P1

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK,
    LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, -1 /* RST handled via TCA9554 */, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// --- AXP2101 I2C ---
#define AXP_ADDR   0x34
#define I2C_SDA    8
#define I2C_SCL    7
#define I2C_FREQ   400000

// AXP2101 registers
#define REG_STATUS0      0x00  // bit 5 = VBUS good
#define REG_STATUS1      0x01  // bits [2:0] charge status
#define REG_VBAT_H       0x34
#define REG_VBAT_L       0x35
#define REG_VBUS_H       0x36
#define REG_VBUS_L       0x37
#define REG_BAT_PCT      0xA4
#define REG_ICC_SET      0x62

// --- Globals ---
static uint32_t lastGoodRead = 0;   // millis of last successful read
static bool axpFound = false;

// Read a single register, returns -1 on failure.
static int axpRead(uint8_t reg) {
    Wire.beginTransmission(AXP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)AXP_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

// Read a 14-bit ADC value from a register pair.
// VBAT: 1mV/LSB, uses bits [5:0] of H and all of L.
// VBUS: uses bits [5:0] of H and all of L, but 0.305mV/LSB (prescaler).
static int readVbat(void) {
    int h = axpRead(REG_VBAT_H);
    int l = axpRead(REG_VBAT_L);
    if (h < 0 || l < 0) return -1;
    return ((h & 0x3F) << 8) | l;  // 1mV per LSB
}
static int readVbus(void) {
    int h = axpRead(REG_VBUS_H);
    int l = axpRead(REG_VBUS_L);
    if (h < 0 || l < 0) return -1;
    int raw = ((h & 0x3F) << 8) | l;
    return (raw * 305) / 1000;  // 0.305mV per LSB → mV
}

static const char* chargeStateStr(int reg01) {
    if (reg01 < 0) return "I2C ERROR";
    switch ((reg01 >> 5) & 0x03) {  // bits [6:5]
        case 0: return "STANDBY";
        case 1: return "CHARGING";
        case 2: return "CHARGE DONE";
        case 3: return "NOT CHARGING";
        default: return "UNKNOWN";
    }
}

// Decode charge current setting register 0x62.
// AXP2101: bits [4:0], steps of 25mA from 0 to 200mA for values 0-8,
// then steps vary. Simplified: value * 25 up to 200, then lookup.
// Actually the AXP2101 ICC register: 0=0mA, 1=0mA, 2=0mA, 3=0mA,
// 4=100mA, 5=125mA, 6=150mA, 7=175mA, 8=200mA...
// Simpler: the datasheet says bits[4:0] map to specific currents.
// Let's use the known mapping:
static int decodeChargeCurrent(int reg) {
    if (reg < 0) return -1;
    reg &= 0x1F;
    // AXP2101 charge current table (mA):
    // 0=0, 1=0, 2=0, 3=0, 4=100, 5=125, 6=150, 7=175, 8=200,
    // 9=300, 10=400, 11=500, 12=600, 13=700, 14=800, 15=900, 16=1000
    static const int table[] = {
        0, 0, 0, 0, 100, 125, 150, 175, 200,
        300, 400, 500, 600, 700, 800, 900, 1000
    };
    if (reg < (int)(sizeof(table) / sizeof(table[0]))) return table[reg];
    return reg * 100; // fallback guess for undocumented values
}

// Draw a row of label: value text.
static void drawRow(int y, const char* label, const char* value, bool error = false) {
    gfx->setCursor(10, y);
    gfx->setTextColor(BLACK);
    gfx->print(label);
    gfx->setCursor(290, y);
    gfx->setTextColor(error ? RED : BLACK);
    gfx->print(value);
}

// TCA9554 helpers
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

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("AXP2101 Charge Diagnostic");

    // I2C init (needed before LCD reset and AXP reads)
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // LCD reset via TCA9554 I/O expander
    lcd_reset();

    // Backlight
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // Display init
    gfx->begin();
    gfx->setRotation(1);  // 90 CW via software
    gfx->fillScreen(WHITE);
    gfx->setTextSize(3);
    gfx->setTextWrap(false);

    // Probe AXP2101
    Wire.beginTransmission(AXP_ADDR);
    if (Wire.endTransmission() == 0) {
        axpFound = true;
        Serial.println("AXP2101 found at 0x34");
    } else {
        axpFound = false;
        Serial.println("AXP2101 NOT FOUND");
        gfx->fillScreen(WHITE);
        gfx->setTextColor(RED);
        gfx->setCursor(30, 130);
        gfx->setTextSize(4);
        gfx->print("AXP2101 NOT FOUND");
        gfx->flush();
        // Halt
        while (true) { delay(1000); }
    }
}

void loop() {
    if (!axpFound) return;

    // Read all values
    int status0  = axpRead(REG_STATUS0);
    int status1  = axpRead(REG_STATUS1);
    int vbatMv   = readVbat();
    int vbusMv   = readVbus();
    int batPct   = axpRead(REG_BAT_PCT);
    int iccReg   = axpRead(REG_ICC_SET);
    int ichgMa   = decodeChargeCurrent(iccReg);

    bool anyError = (status0 < 0 || status1 < 0 || vbatMv < 0 || vbusMv < 0 || batPct < 0);

    if (!anyError) {
        lastGoodRead = millis();
    }

    uint32_t secsSinceUpdate = (millis() - lastGoodRead) / 1000;

    // Serial output
    bool vbusGood = (status0 >= 0) && (status0 & 0x20);
    Serial.printf("VBUS=%dmV CHG=%s VBAT=%dmV PCT=%d ICHG=%dmA\n",
        vbusMv >= 0 ? vbusMv : 0,
        chargeStateStr(status1),
        vbatMv >= 0 ? vbatMv : 0,
        batPct >= 0 ? batPct : 0,
        ichgMa >= 0 ? ichgMa : 0);

    // Redraw screen
    gfx->fillScreen(WHITE);
    gfx->setTextSize(3);

    char buf[64];
    int y = 20;
    int rowH = 46;

    // Row 1: VBUS present
    if (status0 < 0) {
        drawRow(y, "VBUS:", "I2C ERROR", true);
    } else {
        drawRow(y, "VBUS:", vbusGood ? "YES" : "NO");
    }
    y += rowH;

    // Row 2: VBUS voltage
    if (vbusMv < 0) {
        drawRow(y, "VBUS mV:", "I2C ERROR", true);
    } else {
        snprintf(buf, sizeof(buf), "%d.%02d V", vbusMv / 1000, (vbusMv % 1000) / 10);
        drawRow(y, "VBUS mV:", buf);
    }
    y += rowH;

    // Row 3: Charge state
    drawRow(y, "Charge:", chargeStateStr(status1), status1 < 0);
    y += rowH;

    // Row 4: Battery voltage + percentage
    if (vbatMv < 0 || batPct < 0) {
        drawRow(y, "Battery:", "I2C ERROR", true);
    } else {
        snprintf(buf, sizeof(buf), "%d.%02d V  (%d%%)", vbatMv / 1000, (vbatMv % 1000) / 10, batPct);
        drawRow(y, "Battery:", buf);
    }
    y += rowH;

    // Row 5: Charge current
    if (ichgMa < 0) {
        drawRow(y, "Chg mA:", "I2C ERROR", true);
    } else {
        snprintf(buf, sizeof(buf), "%d mA", ichgMa);
        drawRow(y, "Chg mA:", buf);
    }
    y += rowH;

    // Row 6: Last update
    snprintf(buf, sizeof(buf), "%lus ago", secsSinceUpdate);
    drawRow(y, "Updated:", buf);

    gfx->flush();
    delay(1000);
}
