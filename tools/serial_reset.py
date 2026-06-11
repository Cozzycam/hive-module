"""Hard-reset the S3 via an RTS pulse (EN low) with DTR held deasserted,
mirroring esptool's 'hard resetting via RTS pin'. Recovers a module that
got strapped into the bootloader by a bad serial open.
Usage: py serial_reset.py COM3
"""
import sys
import time

import serial

ser = serial.Serial()
ser.port = sys.argv[1]
ser.baudrate = 115200
ser.dtr = False   # IO0 high — stay out of bootloader
ser.rts = False
ser.open()
ser.rts = True    # EN low — chip in reset
time.sleep(0.2)
ser.rts = False   # EN high — boot into app
ser.close()
print("reset pulse sent")
