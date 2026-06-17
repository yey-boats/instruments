#!/usr/bin/env python3
"""Drive the device's bench-sweep over USB serial and capture the CSV table.

Opens the CH340, waits for boot/SignalK, sends `bench-sweep`, and records every
`[bench-sweep]` line for the duration of the sweep (~screens x 2 modes x dwell).
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
RUN_SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 150

ser = serial.Serial(PORT, 115200, timeout=1)
time.sleep(1.0)
ser.reset_input_buffer()

# Probe link/SignalK state first (informational).
ser.write(b"\r\nsk-status\r\n")
ser.flush()
t0 = time.time()
boot_lines = []
while time.time() - t0 < 3:
    line = ser.readline().decode("utf-8", "replace").rstrip()
    if line:
        boot_lines.append(line)
for l in boot_lines[-8:]:
    print("PRE:", l)

# Kick off the sweep.
ser.write(b"bench-sweep\r\n")
ser.flush()
print(">>> sent bench-sweep")

rows = []
t0 = time.time()
done = False
while time.time() - t0 < RUN_SECONDS and not done:
    line = ser.readline().decode("utf-8", "replace").rstrip()
    if not line:
        continue
    if "bench-sweep" in line:
        print(line)
        rows.append(line)
        if "complete" in line:
            done = True

ser.close()
print("\n=== captured %d bench-sweep lines (done=%s) ===" % (len(rows), done))
