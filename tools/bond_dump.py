#!/usr/bin/env python3
"""Send a serial command to a module with DTR/RTS deasserted (safe for the
ESP32-S3 USB-Serial/JTAG port — asserting them can strap the ROM bootloader)
and print whatever comes back. Default command: 'dump bonds'."""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
cmd = sys.argv[2] if len(sys.argv) > 2 else "dump bonds"
listen_s = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False   # set desired idle state BEFORE open so it isn't pulsed
ser.rts = False
ser.open()

time.sleep(0.4)
ser.reset_input_buffer()
ser.write((cmd + "\r\n").encode())
ser.flush()

deadline = time.time() + listen_s
buf = b""
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
ser.close()

sys.stdout.write(buf.decode(errors="replace"))
print(f"\n--- [{port}] {len(buf)} bytes ---")
