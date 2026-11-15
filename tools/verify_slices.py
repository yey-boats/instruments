#!/usr/bin/env python3
"""On-device verification for slices 3/4/6 over USB serial.

Captures boot health (crash signatures), S4 persisted-config restore, SK
liveness, and S3 per-screen subscribe/unsubscribe diffs as we switch screens.
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
ser = serial.Serial(PORT, 115200, timeout=1)


def drain(secs, tag, keep_substr=None):
    out = []
    t = time.time()
    while time.time() - t < secs:
        line = ser.readline().decode("utf-8", "replace").rstrip()
        if not line:
            continue
        out.append(line)
        if keep_substr is None or any(s in line for s in keep_substr):
            print(f"[{tag}] {line}", flush=True)
    return out


def cmd(c):
    ser.write(("\r\n" + c + "\r\n").encode())
    ser.flush()


CRASH = ["rst:", "Guru Meditation", "RTC_SW_SYS_RST", "Backtrace", "abort()", "panic"]
SUBS = ["[sk] subs", "subscribed", "unsubscrib"]
CFG = ["[mgr] restored", "persisted config", "config drift", "skip flash", "config hash"]

print("=== boot / first 14s (crash + config-restore + subs init) ===", flush=True)
boot = drain(14, "boot", keep_substr=CRASH + SUBS + CFG + ["[sk]", "[mgr]", "heap"])

crashes = [l for l in boot if any(c in l for c in CRASH)]
print(f"\n>>> crash signatures in boot window: {len(crashes)}", flush=True)

print("\n=== sk-status ===", flush=True)
cmd("sk-status")
drain(4, "sk")

# S3: switch screens, capture subscription diffs + liveness.
screens = ["nav", "wind", "depth", "steering", "wind_steer", "autopilot", "nav"]
print("\n=== S3: per-screen subscribe/unsubscribe ===", flush=True)
for s in screens:
    cmd("screen " + s)
    print(f"\n--- screen {s} ---", flush=True)
    drain(3, s, keep_substr=SUBS + ["[sk]"])
    cmd("sk-status")
    drain(2, s + ":sk", keep_substr=["connected=", "lastUpdate"])

cmd("screen")
drain(2, "screen")

ser.close()
print("\n=== verification capture done ===", flush=True)
