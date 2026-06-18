# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`esp32-boat-mfd` — PlatformIO/Arduino firmware turning a **Sunton ESP32-4848S040**
(ESP32-S3-N16R8, 4.0″ 480×480 ST7701 RGB panel, GT911 touch) into a SignalK
marine multi-function display. LVGL 9 UI, WiFi+BLE provisioning, ArduinoOTA.
Source-available under PolyForm Noncommercial 1.0.0.

## Build & test commands

The Makefile is the user-facing entry point; targets wrap PlatformIO. Three envs:

| Env | Purpose |
|---|---|
| `esp32-4848s040` | Production firmware build |
| `ota` | Same build, `upload_protocol=espota` for OTA flashing |
| `native` | Host unit tests (Unity). Builds **only** `signalk_parser.cpp` + `layout.cpp`. |

```sh
make build                              # pio run -e esp32-4848s040
make test                               # pio test -e native (parser + layout tests)
make flash                              # USB flash, auto-detects /dev/cu.usbserial-*
make ota DEVICE_IP=<ip>                 # WiFi flash (port 3232)
make monitor                            # serial @ 115200
make ble                                # python BLE console (sends `ip`+`sk-status`, streams logs)
make ble-cmd CMD="sk-status"            # one-shot BLE command
make logs                               # listen on UDP :9999 (mirrored logs)
make demo-up / demo-down                # docker SignalK + tools/fake_boat.py (from ../signalk-espdisp-manager)
make lint / make format                 # clang-format LLVM style + py_compile
make pre-commit                         # same lint command used by hook + CI
make hooks-install                      # set core.hooksPath=.githooks
make backup                             # full 16 MB flash dump via tools/dump_chunked.sh
```

Run a single host test by env-filter on the PIO command:
```sh
pio test -e native -f test_parser       # or test_layout
```

## Architecture

`src/main.cpp` (LVGL UI + display/touch init) is the only TU that depends on
Arduino_GFX and LVGL. All networking, parsing, and config live in side
modules so they can be host-tested:

```
                              +-----------+
                              |  main.cpp |  display + LVGL + touch + UI refresh
                              +-----+-----+
                                    | ui_refresh @ 5 Hz reads sk::data
                                    | dispatch user input -> net::dispatchCommand
                                    v
  +-----------+   +-----------+   +-------------+   +---------------+
  | net.cpp   |-->| signalk   |-->| signalk_    |   | layout_loader |
  | WiFi/BLE/ |   | .cpp WS   |   | parser.cpp  |   | .cpp +        |
  | OTA/mDNS/ |   | client    |   | (pure, host)|   | layout.cpp    |
  | UDP log   |   +-----------+   +-------------+   | (pure, host)  |
  +-----+-----+                                     +---------------+
        |                                                 ^
        +-----> ble_config.cpp (GATT: CONNECTION + CONFIGURATION characteristics)
                                                          |
                                                          +- apply_json() / fetch_from_signalk()
```

Key contracts:

- **`net::dispatchCommand(line)`** is the single command funnel. It tries
  `net::handleSerialCommand` → `sk::handleSerialCommand` →
  `layout::handleSerialCommand` → `s_extra` (main's `handleMainCommand`).
  Serial, BLE NUS RX, and BLE CONNECTION writes (`view`, `id`) all route
  through it. Add new commands by extending one of these handlers, not by
  parsing inside `ble_config.cpp`.
- **`sk::data`** (in `signalk_parser.h`) is the single source of truth for
  live boat state. Parser is pure C++ and tested on the host; the device
  loop only feeds it WebSocket frames.
- **`layout::Config`** (`include/layout.h`) is a ~34 KB POD with fixed
  bounds (`MAX_SCREENS=8`, `MAX_TILES_PER_SCREEN=4`, …). The live copy is
  **heap-allocated in PSRAM** by `layout_loader.cpp` — never make it
  `static` in `.bss` (see "Memory traps" below).
- **`net::deviceId()`** is the NVS-persisted name used for BLE advertising,
  mDNS host, OTA hostname, and WiFi hostname. `id <name>` reboots; default
  is `OTA_HOSTNAME` from `secrets.h`.

## Memory traps (these have all bitten before — preserve fixes)

- **GT911 returns big-endian coordinates on this panel** (contrary to most
  GT911 references). Decode each 16-bit field as
  `((uint16_t)hi << 8) | lo` with explicit ordered reads — never
  `Wire.read() | (Wire.read() << 8)` (unspecified evaluation order).
- **`layout::parse()` must `memset(&out, 0, sizeof(out))`**, not
  `out = Config{}` — the latter creates a 34 KB temporary that overflows
  the 8 KB Arduino main stack and boot-loops the device.
- **The live `layout::Config` must be PSRAM-allocated**
  (`heap_caps_calloc(1, sizeof(Config), MALLOC_CAP_SPIRAM)`). A `static
  Config` in internal SRAM starves NimBLE and the controller hangs
  silently between WiFi AP and BLE init.
- **`NimBLECharacteristic::setValue` overload trap**: always use the
  `(const uint8_t *data, size_t len)` form. `setValue(const char *)` can
  resolve to `setValue(uint32_t)` and store 4 bytes of pointer.
- **BLE attribute values are capped at 512 bytes.** `ConfigCb::onRead`
  returns a JSON summary stub (`{truncated, size, screen_count, ...}`)
  when the layout exceeds the cap; large layouts must come in via the
  SignalK REST endpoint and be triggered with `layout-fetch`.
- **Never build a large struct on a task callback's stack.** The NimBLE
  host task (~4 KB stack), the WebServer task, and even the Arduino
  loopTask (8 KB) are small. A `proto::AttachAck`/`DeviceRecord` (~1.5 KB)
  or `proto::ControlState` (~2 KB) declared as a stack local inside a GATT
  `onRead`/`onWrite`, an HTTP handler, or a controller loop overflows the
  stack and reboots the device with **no panic text** (the crash precedes
  console init: ROM boot → `entry` → `RTC_SW_SYS_RST`, repeating). Make
  such scratch structs `static` (GATT/web callbacks run serially on one
  task, so a function-static is race-free) + `memset` in place, or
  heap/PSRAM. This is the `out = Config{}` 34 KB-temporary trap in a new
  guise — it bit the BLE Control handlers (`ble_config.cpp`), the IP
  `/api/p2p` handlers (`web.cpp`), and the harness loop, all at once, and
  only showed up on hardware (compile + host tests pass).
- **DevKitC-1 N16R8 harness env needs `board_build.arduino.memory_type =
  qio_opi`.** Without it (but with `BOARD_HAS_PSRAM`) the octal-PSRAM
  cache config mismatches and the app faults at its entry point on every
  boot — same silent reboot signature as above. The base board env sets
  it; standalone S3 envs must too.
- **R0–R4 / B0–B4 pin lists were swapped in early references.** The
  verified pin map is in `include/board_pins.h`; do not "fix" it from
  web sources without a camera-based color test.
- **`board_pins.h` ST7701 init table and the NUS UUID `#define` block
  are wrapped in `// clang-format off`/`on`** — keep that protection when
  editing or `make lint` will fail in CI.
- **`ui_markers::draw_glyph` allocates a per-glyph PSRAM canvas buffer that
  is intentionally never freed** (lifetime = the screen session, like all
  built-once screen objects). Marker rings build their glyph canvases at
  screen-build time; `marker_ring_update` never allocates. If you ever add a
  path that *deletes* glyph canvases (dynamic/rebuilt dials), you must
  `heap_caps_free(buf)` the canvas buffer first or it leaks PSRAM — LVGL does
  not own a user-supplied canvas buffer. Keep glyph drawing on the UI task.

## Adding things

### A new SignalK path
1. Field on `sk::Data` in `include/signalk_parser.h`.
2. Path → field branch in `applyValue` in `src/signalk_parser.cpp`.
3. Test case in `test/test_parser/test_parser.cpp` (host-runnable).
4. `subscribe()` entry in `src/signalk.cpp` so the server sends it.
5. Render in `main.cpp::ui_refresh` if it should appear on screen.

### A new board variant
1. `include/board_pins_<board>.h` with full GPIO map.
2. `[env:<board>]` block in `platformio.ini` with a `-D BOARD_<NAME>` flag.
3. Switch on the macro in `main.cpp` to include the right pins file.
4. Add a CI matrix entry in `.github/workflows/ci.yml`.

### A new console command
Pick the right handler: `net` (connectivity/identity), `sk` (SignalK target),
`layout` (layout loader/show/fetch), or `handleMainCommand` in `main.cpp`
(UI actions like `view`, `demo`). It will automatically be reachable from
serial **and** BLE NUS via `net::dispatchCommand`.

## Session workflow (for AI agents)

`CLAUDE-orch.md` at the repo root describes the broader orchestration
pattern (Beads, Gastown, slash-command health workflows). The parts
that apply to *this* firmware repo, distilled:

- **Work is not done until `git push` succeeds.** After committing any
  task, push to origin in the same turn. Leaving commits stranded
  locally counts as incomplete — the next session has no way to know
  they exist. The only exception is when the user explicitly says
  "don't push yet" or similar.
- **Gather context before changing anything.** Read the file you're
  about to edit, grep for the symbol's other call sites, check `git log`
  for recent activity in that area. This is doubly important for the
  memory-trap-laden modules (see above).
- **Library-first for >20 lines of new logic.** Check if the
  PlatformIO/Arduino ecosystem already has a maintained library before
  writing it from scratch.
- **Do, don't ask.** Anything the agent can do unattended (commit,
  push, lint, build, run tests, OTA-flash via `make ota-verify`, recover
  the device via `python3 tools/espdisp.py recover`) should happen
  without prompting the user. Only ask when a decision is genuinely
  the user's (security tradeoff, design choice, missing credential).
- **Verify after acting.** Read the modified file. Run `make pre-commit`
  + `pio test -e native` + a build for the affected env before claiming
  the change works. For firmware changes, `make ota-verify` confirms
  the new binary actually booted on the device.
- **If the next stage is doable, do it.** Don't stop at "could now do X" —
  if X is unambiguous, unattended, and has no decision the user owns,
  do X and continue to the stage after. A reasonable stopping point is
  when you hit a genuine blocker (missing creds, ambiguous design call,
  external service down) or when the user has explicitly scoped the
  ask to a single step. "I finished step 1; want me to do step 2?" is
  almost always the wrong end-of-turn — just do step 2.

`CLAUDE-orch.md` itself is generic across projects in this fleet; the
firmware-specific contracts (memory traps, dispatch funnel, layout
PSRAM rule) in this file win where the two overlap.

## Conventions

- **Commits**: Conventional Commits (`feat:`, `fix:`, `refactor:`, `docs:`,
  `chore:`, `test:`) — used by the `release.yml` workflow to generate
  release notes from a tag.
- **Code style**: C++17, LLVM-ish, 4-space indent, brace on same line.
  `make format` runs clang-format in place. Run `make pre-commit` before
  committing; CI and `.githooks/pre-commit` use the same target, so do not
  skip lint locally and leave CI to fail on formatting.
- **Licensing/identity**: project is attributed to **"navado and
  contributors"** — do not insert real names in headers, license, or
  README. License is **PolyForm Noncommercial 1.0.0**; do not relicense
  or add an OSI-OSS dual-license without explicit instruction.
- **Releases**: `make release-tag VERSION=v0.x.y` then `git push origin
  v0.x.y`. Tags matching `*-rc*`/`*-alpha*`/`*-beta*` are auto-marked
  pre-release.

## Demo / SignalK auth note

The SignalK manager plugin and its deployment (test-server config, Docker
compose, `run.sh`/`run-remote.sh`/`stop*.sh`) now live in
**yey-boats/Instruments-manager** — clone it next to this repo as
`../signalk-espdisp-manager` (or set `MANAGER_DIR`). The `make demo-*`
targets call its `deploy/scripts/`.

`tools/fake_boat.py` and the firmware both authenticate against SignalK
with a token (`?token=` on the WebSocket). The bundled `signalk/signalk-server`
Docker image rejects anonymous writes by default — first run creates the
admin user (the demo scripts assume username `admin`, password `admin`).

## Recovery notes

- Bad WiFi credentials or a runaway OTA can wedge the device; recovery is
  `esptool erase_region 0x9000 0x5000` (NVS) over USB, then re-provision.
- The CH340 USB-UART is flaky; cycle the cable before assuming a
  firmware/serial problem.
- iOS Personal Hotspot blocks mDNS — use the raw IP for `make ota`.
