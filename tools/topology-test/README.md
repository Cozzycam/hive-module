# Topology Detection Bench Prototype

Standalone PlatformIO project testing the per-face DETECT pin + ESP-NOW handshake scheme for inter-module topology detection.

## Hardware Setup

Both boards run the same firmware and share GND (via VBUS bus or dedicated jumper).

**DETECT_PIN: GPIO 18** (change in `main.cpp` if needed)

### Single-jumper test (asymmetric)

```
Board B GND ----[jumper]----> Board A GPIO 18
```

- Board A sees DETECT go LOW, broadcasts HELLO
- Board B receives HELLO, replies with REPLY
- Both transition to CONNECTED

### Two-jumper test (bidirectional)

```
Board B GND ----[jumper]----> Board A GPIO 18
Board A GND ----[jumper]----> Board B GPIO 18
```

- Both boards see DETECT go LOW simultaneously
- Both broadcast HELLO, both reply
- Both transition to CONNECTED

## Building & Flashing

```bash
# Board A (adjust COM port)
cd tools/topology-test
pio run -e esp32s3 --upload-port COM4 -t upload

# Board B
pio run -e esp32s3 --upload-port COM3 -t upload
```

## Serial Monitor

115200 baud. Logs every state transition and ESP-NOW message.

```bash
pio device monitor --port COM4 --baud 115200
```

## Display

480x320 landscape, 6 rows:
- My ID, DETECT pin state, State machine, Neighbour ID, Last event, Uptime

## Protocol

Messages broadcast via ESP-NOW:
- **HELLO** — "I detected a neighbour, who are you?"
- **REPLY** — "I'm 0xXXXX, acknowledged"
- **GOODBYE** — "neighbour disconnected"
- **HEARTBEAT** — periodic ping (1s while connected, 3s timeout)
