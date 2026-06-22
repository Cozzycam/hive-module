#!/usr/bin/env python3
"""Send one serial command to a module and print its output for a few seconds.

DTR/RTS held deasserted before open so the ESP32-S3 USB-Serial/JTAG port isn't
strapped into the ROM bootloader (same guard as bond_dump.py / telemetry_pull.py).

Usage:
    py tools/serial_cmd.py PORT "command" [READ_SECONDS]

    py tools/serial_cmd.py COM3 "warp status"
    py tools/serial_cmd.py COM3 "blankslate" 5
    py tools/serial_cmd.py COM4 "warp 24"
"""
import sys, time, serial

port   = sys.argv[1]
cmd    = sys.argv[2]
read_s = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False   # idle state set BEFORE open so it isn't pulsed (no bootloader strap)
ser.rts = False
ser.open()

time.sleep(0.4)
ser.reset_input_buffer()
ser.write((cmd + "\r\n").encode())
ser.flush()

end = time.time() + read_s
while time.time() < end:
    data = ser.read(4096)
    if data:
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()
ser.close()
