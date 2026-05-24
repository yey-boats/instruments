# esp32-boat-mfd

[![CI](https://github.com/navado/esp32-boat-mfd/actions/workflows/ci.yml/badge.svg)](https://github.com/navado/esp32-boat-mfd/actions/workflows/ci.yml)
[![Release](https://github.com/navado/esp32-boat-mfd/actions/workflows/release.yml/badge.svg)](https://github.com/navado/esp32-boat-mfd/actions/workflows/release.yml)
[![License: PolyForm-NC-1.0.0](https://img.shields.io/badge/license-PolyForm--NC--1.0.0-yellow.svg)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-6.x-orange.svg)](https://platformio.org)
[![Board: ESP32-4848S040](https://img.shields.io/badge/board-ESP32--4848S040-blue.svg)](#hardware)
[![SignalK](https://img.shields.io/badge/SignalK-client-1eb2a8.svg)](https://signalk.org)

A source-available marine multi-function display (MFD) firmware for the
ESP32-S3 family of touch panels. Acts as a [SignalK](https://signalk.org)
WebSocket client and renders live navigation data on an LVGL dashboard.

Licensed under [PolyForm Noncommercial 1.0.0](LICENSE) — free for
personal, research, educational, and other noncommercial use.
**Commercial use requires a separate license** (see [Commercial use](#commercial-use)).

<p align="center">
  <a href="docs/demo.mp4">
    <img src="docs/demo.gif" alt="Live dashboard demo" width="320">
  </a>
  <br>
  <em>Live SignalK data — wind, navigation, depth, position, battery. Click for full-quality MP4.</em>
</p>

## Features

- **SignalK over WebSocket** — subscribes to navigation, wind, depth, water temperature, battery, and tank levels
- **LVGL 9 dashboard** — 4-quadrant layout on 480×480 IPS, 5 Hz refresh
- **WiFi provisioning** — STA mode with AP fallback for first-time setup, credentials persisted in NVS
- **Over-the-air updates** — ArduinoOTA on port 3232 (no USB cable for iteration)
- **BLE diagnostics** — Nordic UART service for logs + console commands without WiFi
- **Multi-target logging** — Serial / UDP broadcast / BLE notify, the same `logf()` writes to all three
- **Host-portable parser** — SignalK delta logic builds and tests on macOS / Linux as well as the device
- **CI + release automation** — GitHub Actions builds firmware on every push and attaches binaries to tagged releases

## Hardware

Primary target:

| Component | Detail |
|-----------|--------|
| Board | Sunton / Guition **ESP32-4848S040** (also labelled `ESP32-4840S040`) |
| MCU | ESP32-S3-WROOM-1 **N16R8** — 16 MB flash + 8 MB octal PSRAM |
| Display | 4.0″ IPS 480×480, ST7701 RGB parallel |
| Touch | GT911 capacitive, I²C `SDA=19 SCL=45` |
| Storage | microSD slot |
| USB | USB-C with CH340 USB-UART |

The codebase is structured so that adding another ESP32-S3 + RGB-panel + GT911
board is mostly a matter of dropping in a new `include/board_pins_*.h`
and a `[env:*]` block in `platformio.ini`.

## Quick start

```sh
# 1. Clone and set up
git clone https://github.com/navado/esp32-boat-mfd.git
cd esp32-boat-mfd
make setup

# 2. First flash over USB (CH340 driver required: silabs.com / wch-ic.com)
make flash

# 3. Provision WiFi over the serial console (in a separate terminal)
make monitor
#   wifi <ssid> <password>

# 4. Read the device's IP off the serial log, then iterate over WiFi
make ota DEVICE_IP=10.0.0.42
```

For a no-boat demo run, see [Running with synthetic data](#running-with-synthetic-data).

## Make targets

```
make help          List all targets
make setup         First-time setup (PlatformIO check + secrets.h)
make build         Build firmware
make test          Run host-side unit tests
make flash         Flash over USB (auto-detects /dev/cu.usbserial-*)
make ota           Flash over WiFi   (requires DEVICE_IP=<ip>)
make monitor       Open serial monitor
make ble           Open BLE console (logs + commands without WiFi)
make logs          Listen for UDP log broadcasts on :9999
make demo-up       Start SignalK + synthetic data in Docker
make demo-down     Stop the demo stack
make lint          Check formatting + Python syntax
make format        Auto-format C++ sources
make backup        Dump device flash to backup/full_flash_16MB.bin
make release-tag   Tag a release locally (VERSION=v0.1.0)
make clean         Remove build artifacts
```

## Console commands

Send these over the serial monitor (`make monitor`) or BLE (`make ble`):

| Command | Effect |
|---------|--------|
| `wifi <ssid> <pass>` | Save WiFi credentials and reboot |
| `wifi-forget` | Clear credentials, fall back to AP `espdisp-setup` |
| `ip` | Print current IP / mode / RSSI |
| `scan` | List visible 2.4 GHz networks |
| `sk <host> [port]` | Save SignalK server target and reboot |
| `sk-status` | Print SignalK connection state + age of last delta |
| `sk-dump` | Print currently-parsed values of every tracked field |
| `reboot` | Soft restart |

## BLE access

The device advertises as `espdisp` with **two** GATT services:

### 1. Nordic UART (text console)

UUID `6E400001-B5A3-F393-E0A3-9F4DD9E3A05A` — line-oriented, same commands
as the serial console. Subscribe to TX `6E400003-…` for streamed logs;
write UTF-8 lines to RX `6E400002-…`.

```sh
make ble                       # sends `ip` + `sk-status`, then streams logs
make ble-cmd CMD="sk-status"   # one-shot command
```

### 2. boat-mfd config service (structured)

Service UUID `a3f7e000-7a6b-4f47-b3a5-c4d2e5f6a000` — intended for a
companion mobile app (task #26).

| Characteristic | UUID suffix | Props | Payload |
|---|---|---|---|
| **CONNECTION** | `…e001…` | Read · Write · Notify | JSON: `{ "wifi": {ssid, ip, rssi, mode}, "sk": {host, port, state}, "device": {uptime_ms, heap_free, psram_free} }` |
| **CONFIGURATION** | `…e003…` | Read · Write · Notify | Layout JSON (same schema as the SignalK resource at `configuration.boat-mfd.layouts`), up to 512 B |

Write to CONNECTION with a partial JSON to update WiFi or SignalK target:

```jsonc
{ "wifi": { "ssid": "MyHomeNet", "password": "secret" } }   // saves + reboots
{ "wifi": { "forget": true } }                              // clears creds + reboots
{ "sk": { "host": "192.168.1.100", "port": 3000 } }          // saves + reboots
```

Write to CONFIGURATION with a complete layout JSON to replace the live config.
Reads return the last successfully applied document **only if it fits in 512
bytes** (the BLE attribute-value cap per the BT spec). Larger layouts return
a JSON summary stub:

```json
{ "truncated": true, "size": 917, "screen_count": 1, "alarm_count": 2,
  "default_screen": "dashboard" }
```

For full-layout transfer above 512 B, smartphone apps should use SignalK's
REST endpoint (`PUT /signalk/v1/api/vessels/self/configuration/boat-mfd/layouts/value`)
and trigger a re-load via the device's `layout-fetch` command. Native BLE
chunked transfer is on the roadmap (see task #20).

## Running with synthetic data

To exercise the firmware without a boat:

```sh
make demo-up
#   - starts signalk/signalk-server in Docker on :3000
#   - launches tools/fake_boat.py that pushes sinusoidal nav data
make demo-down
```

`fake_boat.py` connects to SignalK as an authenticated provider and emits
deltas for navigation, wind, depth, water temperature, battery, and tanks
once per second.

## Layout configuration (work in progress)

Multi-screen layouts are described by a JSON document on the SignalK server
(`configuration.boat-mfd.layouts`). The device fetches the config at boot,
falls back to a baked-in default if unreachable, and re-fetches on reconnect.

### Schema

```jsonc
{
  "version": 1,
  "settings": {
    "default_screen": "dashboard",
    "demo_period_ms": 3000
  },
  "screens": [
    {
      "id": "dashboard",
      "title": "Dashboard",
      "type": "quadrants",
      "tiles": [
        {
          "id": "wind",
          "title": "WIND",
          "type": "wind",
          "paths": {
            "awa": "environment.wind.angleApparent",
            "aws": "environment.wind.speedApparent"
          }
        }
      ]
    },
    {
      "id": "steering",
      "title": "Steering",
      "type": "steering",
      "paths": {
        "hdg": "navigation.headingTrue",
        "cts": "navigation.courseRhumbline.courseToSteer",
        "xte": "navigation.courseRhumbline.crossTrackError"
      }
    }
  ],
  "alarms": [
    {
      "id": "shallow",
      "path": "environment.depth.belowTransducer",
      "level": "alarm",
      "lt": 3.0,
      "message": "SHALLOW WATER"
    }
  ]
}
```

### Field reference

| Field | Allowed values |
|-------|----------------|
| `screens[].type` | `quadrants` &middot; `steering` &middot; `autopilot` &middot; `route` &middot; `trip` &middot; `chart` |
| `screens[].tiles[].type` | `wind` &middot; `nav` &middot; `depth_temp` &middot; `device_status` &middot; `big_number` &middot; `compass` |
| `alarms[].level` | `info` &middot; `warn` &middot; `alarm` &middot; `emergency` |
| `alarms[].lt` / `.gt` | Number — trigger when the path's value crosses below `lt` or above `gt` |

Bounds (compile-time, see `include/layout.h`): max 8 screens, 4 tiles per
screen, 6 path bindings per object, 8 alarms. Strings truncate to 32
chars for ids/titles, 96 for SignalK paths.

### Status

- Schema defined + host-portable parser with 9 unit tests passing — `include/layout.h`, `src/layout.cpp`
- Fetcher (SignalK REST) and LVGL renderer not yet wired (tracked in task #7)

## Architecture

```
                 +-------------------+
                 |  ESP32-4848S040   |
                 |  ESP32-S3-N16R8   |
                 +---------+---------+
                           |
        +---- WiFi --------+--------- BLE --------+
        |                                         |
        v                                         v
 SignalK WebSocket                          Nordic UART
 ws://host:3000/                            (logs + commands)
 signalk/v1/stream
        |
        v
 +------+---------+
 | signalk_parser | -- applyDelta(json, Data) ----> sk::data
 +----------------+
        |
        v
 +----------------+
 |   LVGL UI      | -- 5 Hz refresh from sk::data
 |  4 quadrants   |
 +----------------+
```

| File | Purpose |
|------|---------|
| `src/main.cpp` | Display + touch init, LVGL UI, main loop |
| `src/net.cpp` | WiFi STA/AP, ArduinoOTA, mDNS, BLE GATT, multi-target logging |
| `src/signalk.cpp` | WebSocket client, subscription, NVS-persisted target |
| `src/signalk_parser.cpp` | Pure delta parser (host-portable, unit tested) |
| `include/board_pins.h` | GPIO map for the supported board |
| `include/lv_conf.h` | LVGL build configuration |
| `include/secrets.h.example` | Template for WiFi/OTA credentials |
| `tools/ble_console.py` | BLE debug / config tool |
| `tools/fake_boat.py` | Synthetic SignalK data pusher |
| `tools/dump_chunked.sh` | Chunked, resumable full flash backup |

## Testing

```sh
make test
```

Unit tests live under `test/test_parser/` and run under PlatformIO's `native`
environment (Unity + ArduinoJson). The parser deliberately has no Arduino
dependencies, so the same code path that runs on the device is exercised on
the CI host. Tests cover every supported SignalK path, partial / malformed
payloads, and keep-alive frames.

## Releasing

Maintainers cut releases by tagging:

```sh
make release-tag VERSION=v0.1.0
git push origin v0.1.0
```

The `release.yml` workflow builds the firmware on push of a `v*` tag,
attaches `firmware.bin`, `merged_firmware.bin`, ELF, and SHA-256 sums to
the GitHub release, and generates release notes from commits since the
previous tag.

Pre-releases are detected automatically: tags matching `*-rc*`, `*-alpha*`,
or `*-beta*` are marked as pre-release.

## Roadmap

- [ ] Move position (lat/lon) into the Nav quadrant; promote Status to a device-health panel
- [ ] Multi-screen layouts with server-managed configuration (JSON document on SignalK)
- [ ] Triple-tap to expand a tile to fullscreen; triple-tap again to restore
- [ ] Swipe gestures to scroll between screens
- [ ] Advanced screens: compass rose, AIS targets, engine, anchor watch, tank levels, history graphs
- [ ] Raster chart display fed by a SignalK charts plugin
- [ ] NMEA 0183 input via RS-422 transceiver on a free UART
- [ ] Optional NMEA 2000 (CAN) support
- [ ] NVS caching of last-known config so the device boots into the right layout without network

## Related projects

| Project | Scope | License |
|---------|-------|---------|
| [`pypilot/pypilot_mfd`](https://github.com/pypilot/pypilot_mfd) | ESP32-S3 MFD: NMEA 0183 + SignalK + pypilot integration | GPLv3 |
| [`mxtommy/Kip`](https://github.com/mxtommy/Kip) | Web-based SignalK instrument package | — |
| [`mrstas/SC01_PLUS_MARINE_INSTRUMENTS`](https://github.com/mrstas/SC01_PLUS_MARINE_INSTRUMENTS) | SignalK instruments on Panlee SC01 Plus | GPLv3 |
| [`SignalK/SensESP`](https://github.com/SignalK/SensESP) | Sensor-side ESP32 framework (good companion) | Apache 2.0 |
| [`open-boat-projects-org/esp32-nmea2000-obp60`](https://github.com/open-boat-projects-org/esp32-nmea2000-obp60) | N2K gateway with OBP60 e-ink display | — |

## Contributing

Bug reports, board ports, and PRs welcome. See [CONTRIBUTING.md](CONTRIBUTING.md)
for development workflow and conventions. By contributing, you agree that
your contributions are licensed under PolyForm Noncommercial 1.0.0 (the
project license).

## Commercial use

This firmware is **not** licensed for commercial use under the default terms.
"Commercial use" includes selling the firmware, bundling it with hardware sold
for profit, integrating it into a paid service, or using it as part of a
commercial operation (e.g. charter fleets, paid installations).

For commercial licensing, open a
[GitHub Discussion](https://github.com/navado/esp32-boat-mfd/discussions) or
file an issue marked `licensing` to start the conversation.

Noncommercial uses — personal boats, research, education, charitable and
governmental organizations — are explicitly permitted under the project
license.

## License

[PolyForm Noncommercial 1.0.0](LICENSE) © 2026 navado and contributors.

This project bundles and links against the following libraries, each under
its own license:

| Library | License |
|---------|---------|
| LVGL | MIT |
| Arduino_GFX | MIT |
| NimBLE-Arduino | Apache 2.0 |
| WebSockets | LGPL-2.1 |
| ArduinoJson | MIT |
| Arduino-ESP32 | LGPL-2.1 |

These are unmodified upstream dependencies and remain governed by their
respective licenses.
