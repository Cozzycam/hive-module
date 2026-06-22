#!/usr/bin/env python3
"""Poll COM3/COM4 until their `warp` runs finish, then pull needs+bonds CSVs.

Run in the background; it blocks ~10 min while the warps complete on-device, then
saves warp_run_<port>.csv / warp_run_<port>_bonds.csv at the repo root.
"""
import subprocess, time, serial, os, sys

ROOT  = "C:/claude/hive-module"
PULL  = ROOT + "/tools/telemetry_pull.py"
PORTS = ["COM3", "COM4"]
LABEL = sys.argv[1] if len(sys.argv) > 1 else "warp_run"   # output filename prefix

def warp_is_on(port):
    try:
        s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
        s.dtr = False; s.rts = False; s.open()
        time.sleep(0.3); s.reset_input_buffer()
        s.write(b"warp status\r\n"); s.flush()
        end = time.time() + 2.0; buf = b""
        while time.time() < end:
            buf += s.read(4096)
        s.close()
        for ln in buf.decode(errors="replace").splitlines():
            if "[warp]" in ln and ("ON " in ln or "off " in ln):
                return "ON " in ln
        return None
    except Exception:
        return None

deadline = time.time() + 1500
while time.time() < deadline:
    states = {p: warp_is_on(p) for p in PORTS}
    print(f"[wait] {states}", flush=True)
    if all(states[p] is False for p in PORTS):
        print("[wait] both warps OFF — pulling", flush=True)
        break
    time.sleep(25)

for port in PORTS:
    sfx = port.lower()
    subprocess.run(["py", PULL, port, f"{ROOT}/{LABEL}_{sfx}.csv", "needs"])
    subprocess.run(["py", PULL, port, f"{ROOT}/{LABEL}_{sfx}_bonds.csv", "bonds"])
print("DONE", flush=True)
