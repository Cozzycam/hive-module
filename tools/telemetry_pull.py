#!/usr/bin/env python3
"""Pull a module's tuning telemetry CSV over serial and save it to a file.

Sends `dump telemetry`, reads until the firmware's `=== END ===` marker (or an
idle timeout), strips the framing lines, and writes the CSV out. DTR/RTS are held
deasserted before open so the ESP32-S3 USB-Serial/JTAG port isn't strapped into
the ROM bootloader (same guard as bond_dump.py).

Usage:
    py tools/telemetry_pull.py [PORT] [OUTFILE] [IDLE_TIMEOUT_S]// defaults below

    py tools/telemetry_pull.py COM3 com3_telemetry.csv
    py tools/telemetry_pull.py COM4 com4_telemetry.csv

For a multi-MB file the dump is slow (~10 KB/s over serial) — that's fine for an
occasional pull. If you want the whole multi-day file fast, just eject the SD
card and copy /colony/telemetry/*.csv directly.
"""
import sys, time, serial

port    = sys.argv[1] if len(sys.argv) > 1 else "COM3"
outfile = sys.argv[2] if len(sys.argv) > 2 else f"{port}_telemetry.csv"
idle_s  = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False   # set desired idle state BEFORE open so it isn't pulsed
ser.rts = False
ser.open()

time.sleep(0.4)
ser.reset_input_buffer()
ser.write(b"dump telemetry\r\n")
ser.flush()

buf = b""
last_rx = time.time()
while True:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        last_rx = time.time()
        if b"=== END ===" in buf:
            break
    elif time.time() - last_rx > idle_s:
        print(f"[warn] idle for {idle_s}s with no END marker — saving what we got")
        break
ser.close()

text = buf.decode(errors="replace")

# Strip framing: keep everything between the header banner and the END marker.
start = text.find("\n", text.find("=== TELEMETRY")) if "=== TELEMETRY" in text else -1
end   = text.find("=== END ===")
csv   = text[start + 1 : end] if (start != -1 and end != -1) else text
csv   = csv.replace("\r\n", "\n").strip("\n") + "\n"

with open(outfile, "w", newline="") as fh:
    fh.write(csv)

rows = max(0, csv.count("\n") - 1)  # minus header
print(f"--- [{port}] {len(buf)} bytes received, ~{rows} data rows -> {outfile} ---")
