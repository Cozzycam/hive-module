"""Send a serial command to the queen and stream output for a while.
Opens the port with DTR/RTS deasserted so the S3 isn't bootloader-strapped.
Usage: py serial_push.py COM3 "push" [listen_seconds]
"""
import sys
import time

import serial

port = sys.argv[1]
cmd = sys.argv[2] if len(sys.argv) > 2 else ""
listen_s = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.5
ser.dtr = False
ser.rts = False
ser.open()

if cmd:
    ser.write((cmd + "\r\n").encode())

end = time.time() + listen_s
while time.time() < end:
    line = ser.readline()
    if line:
        try:
            print(line.decode(errors="replace").rstrip())
        except Exception:
            pass
ser.close()
