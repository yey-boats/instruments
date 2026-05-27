# 07 Multi-source failover (live demo)

**Goal**: visually confirm that the on-screen value tracks the
highest-priority *fresh* source: NMEA2000 > NMEA-WiFi > SignalK.

The unattended `test_source_priority.py` covers the data-layer
transitions; this attended test validates the user-visible behavior.

## Setup

In three terminals:

```sh
# T1 - device console
make ble

# T2 - SignalK demo + slow synthetic data
make demo-up
python3 tools/fake_boat.py
# fake_boat publishes SOG ~3-5 kn

# T3 - NMEA-WiFi UDP injector (held until you ctrl-C)
python3 - <<'PY'
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
def cksum(b):
    h=0
    for c in b.encode(): h^=c
    return f"{h:02X}"
def rmc(sog_kn):
    body=f"GPRMC,123519,A,4807.0380,N,01131.0000,E,{sog_kn:.1f},084.4,230394,003.1,W"
    return f"${body}*{cksum(body)}\r\n"
while True:
    msg = rmc(15.0)
    s.sendto(msg.encode(), ("255.255.255.255", 10110))
    time.sleep(0.5)
PY
```

## Steps

1. On dashboard, with T2 running only, **SOG ≈ 3-5 kn** (SignalK
   source). ⬜
2. In T1: `nmea-wifi udp 10110`. ⬜
3. Start T3 (the UDP RMC at 15 kn). The dashboard SOG should jump
   to **~15 kn** within ~1 s. ⬜
4. Verify the source switch via T1:
   ```
   boat
   ```
   `sog_mps` should now read `source=nmea-wifi`. ⬜
5. Ctrl-C T3. Within `nmea_wifi_ms` (default 3 s) the SOG should
   fall back to ~3-5 kn (SignalK reclaims). `boat` shows
   `source=signalk`. ⬜
6. In T1: `nmea-wifi disable` to clean up. ⬜

## Pass criteria

Visual SOG follows the same priority chain the data layer enforces.

## If it fails

- SOG never changes → the device might not have heard the UDP
  broadcasts. Confirm both runner and device are on the same L2
  segment; the test broadcasts to `255.255.255.255`.
- Switch takes too long → `boat timeout wifi 1000` to tighten the
  fallback window.
