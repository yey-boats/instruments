# KDCube modeled SignalK simulator (lab)

The lab now drives SignalK from the **KDCube boat simulator**
([navigator-tg-bot `signalk/sim`](https://github.com/kdcube/navigator-tg-bot/tree/main/signalk/sim))
instead of the old `tools/fake_boat.py`. It's a route-following Adriatic boat
with polar/weather/navigator physics that emits a full modeled dataset at 1 Hz:
SOG, STW (SOW), heading/COG, apparent + true wind (AWA/AWS/TWA/TWS), depth,
swell, temperatures, electrical, **autopilot** (`steering.autopilot.*`), and —
via the local patch below — **tidal current** (`environment.current`).

## Deployment (mythra-nav)

```
# on mythra-nav
cd /home/compulab/espdisp-signalk/kdcube-sim/sim          # the signalk/sim subtree
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt                            # numpy httpx lxml websockets
```

Runs as the **`kdcube-sim.service`** systemd unit (replaces the disabled
`fake-boat.service`):

```
WorkingDirectory=/home/compulab/espdisp-signalk/kdcube-sim/sim
Environment=SIGNALK_HOST=localhost SIGNALK_PORT=3000 SIGNALK_USERNAME=admin SIGNALK_PASSWORD=admin
ExecStart=.../.venv/bin/python simulator.py
Restart=always
```

`systemctl status kdcube-sim` to check; logs via `journalctl -u kdcube-sim`.
Data files (route KMZ, polar CSV, cached depth profile) ship in the subtree, so
no network fetch is needed on start.

## Local patch: modeled tidal current

Upstream emits wind + nav + autopilot but not `environment.current`. The
device's wind-screen current vector needs it, so `modules/signalk_writer.py`
gains a modeled set/drift (set swings around the 150° Adriatic drift; drift
breathes 0..~0.8 kn and periodically dips to ~0 to exercise the calm/zero-ring
state). The diff is kept in `kdcube-current-emission.patch` (apply against an
upstream `signalk/sim/modules/signalk_writer.py`).

## Verified

SignalK self vessel reports modeled `environment.wind.*`,
`navigation.speedOverGround/ speedThroughWater`, `environment.current.setTrue/
drift`, and `steering.autopilot.state`. The device picks these up over its
existing subscription (the wind screen's tide vector + small wind/compass
widgets render the modeled current/wind).
