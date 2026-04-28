# AXP2101 Charge Diagnostic

Standalone bench tool for the Waveshare ESP32-S3-Touch-LCD-3.5B.
Reads AXP2101 PMIC registers over I2C and displays charge state on screen.

## Usage

```
cd tools/charge-diagnostic
pio run -t upload
pio device monitor
```

Configured for COM3 by default. Edit `platformio.ini` to change the port.

## What it shows

- VBUS present (YES/NO)
- VBUS voltage
- Charge state (CHARGING / NOT CHARGING / CHARGE DONE / STANDBY)
- Battery voltage and percentage
- Charge current setting
- Seconds since last successful I2C read

All values also printed to serial at 115200 baud.
