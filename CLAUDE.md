# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**Yey Boats Instruments** — PlatformIO/Arduino firmware turning a **Sunton ESP32-4848S040**
(ESP32-S3-N16R8, 4.0″ 480×480 ST7701 RGB panel, GT911 touch) into a SignalK
marine multi-function display. LVGL 9 UI, WiFi+BLE provisioning, ArduinoOTA.
Source-available under PolyForm Noncommercial 1.0.0.

## Build & test commands

The Makefile is the user-facing entry point; targets wrap PlatformIO. Main envs (every board has its own env in `platformio.ini`):

| Env | Purpose |
|---|---|
| `esp32-4848s040` | Production firmware build |
| `ota` | Same build, `upload_protocol=espota` for OTA flashing |
| `native` | Host unit tests (Unity). Builds the pure-C++ side modules (~30 TUs — parser, boat_data, MIDL solve/validate/render mapping, record store, …) per `[env:native] build_src_filter`. |
| `sim-midl` / `sim-screens` / `sim-control` | Headless LVGL host renders: MIDL docs through the real render path, the system touch screens, the control-frame overlay (`make sim`). |

```sh
make build                              # pio run -e esp32-4848s040
make test                               # pio test -e native (host unit tests)
make flash                              # USB flash, auto-detects /dev/cu.usbserial-*
make ota DEVICE_IP=<ip>                 # WiFi flash (port 3232)
make monitor                            # serial @ 115200
make ble                                # python BLE console (sends `ip`+`sk-status`, streams logs)
make ble-cmd CMD="sk-status"            # one-shot BLE command
make logs                               # listen on UDP :9999 (mirrored logs)
make demo-up / demo-down                # docker SignalK + yey-boats-sim (from ../signalk-espdisp-manager)
make lint / make format                 # clang-format LLVM style + py_compile
make pre-commit                         # same lint command used by hook + CI
make hooks-install                      # set core.hooksPath=.githooks
make backup                             # full 16 MB flash dump via tools/dump_chunked.sh
```

Run a single host test by env-filter on the PIO command:
```sh
pio test -e native -f test_parser       # or test_midl_records, test_knob_menu, ...
```

## Architecture

`src/main.cpp` (LVGL UI + display/touch init) and `src/ui/*` are the TUs
that depend on Arduino_GFX and LVGL. All networking, parsing, and config live
in side modules so they can be host-tested.

**The display renders MIDL screens only** (contract:
`docs/midl/midl-only.md`). There are no hand-written C++ content screens, no
`layout::Config` pipeline and no manager-pushed widget plan.

```
  net.cpp / signalk.cpp / sources ──> boat data (sk::data, boat::View)
                                              │ 5 Hz ui_refresh
  Display Manager ── push (HTTP) ──┐          v
        (or pull: config `midl`) ──┼─> midl_store.cpp ──> midl_store_ui.cpp ──> midl_render_apply.cpp
  embedded records (midl/library/  │   records + index     activate / nav       solve -> map_element
     builtin, per display class) ──┘   on LittleFS         (UI task only)       -> ui_layouts.cpp painters
```

- **Records**: `include/midl_store.h` (device half; LittleFS
  `/midl/index.json`, `/midl/records/<id>.json`, `.lkg.json`) and
  `include/midl_records.h` (pure, host-tested rules: override, screen-id
  conflicts, carousel order, fallback chain). Embedded records are generated
  by `tools/embed_builtin_records.py` (`make embed-records`) into
  `include/midl_gen/<class>/builtin_records.h`.
- **One active record**: `midl::store_ui::show(id)` loads the owning record
  first; `next`/`prev` cross record boundaries; `home` = `index.defaultScreen`.
  Every navigation (swipe, `screen` command, HTTP `/api/screen/<id>`, knob)
  goes through `app::CommandType::ShowScreen` on the UI task.
- **System UI** stays native: settings, wifi, touch_cal, touch_grid
  (`include/screens.h`), registered as system screens so
  `ui::reset_screens()` keeps them; plus the lv_layer_top() overlays (MOB,
  alarm banner, control frame, the knob's rotary menu).
- **Knob** (`BOARD_ID_WAVESHARE_KNOB_1_8`): runs its embedded round-360
  records (ap_hud, knob_compass, knob_wind, knob_big). `src/knob_menu.cpp`
  (pure, host-tested) is the rotary state machine: Home turns through the
  MIDL carousel, Steer adjusts the autopilot, Menu/ModePicker/Displays are
  overlay levels drawn by `src/ui/knob_ui.cpp`.

Key contracts:

- **`net::dispatchCommand(line)`** is the single command funnel. It tries
  `net::handleSerialCommand` → `sk::handleSerialCommand` → … → `s_extra`
  (main's `handleMainCommand`). Serial, BLE NUS RX, and BLE CONNECTION writes
  all route through it. Add new commands by extending one of these handlers,
  not by parsing inside `ble_config.cpp`.
- **`sk::data`** (in `signalk_parser.h`) is the single source of truth for
  live boat state. Parser is pure C++ and tested on the host; the device
  loop only feeds it WebSocket frames.
- **LVGL is UI-task only.** Web, BLE and manager handlers validate/persist
  through `midl::store` (mutex-guarded, PSRAM) and queue rendering
  (`MidlActivate` / `ShowScreen`); they never touch LVGL.
- **`net::deviceId()`** is the NVS-persisted name used for BLE advertising,
  mDNS host, OTA hostname, and WiFi hostname. `id <name>` reboots; default
  is `OTA_HOSTNAME` from `secrets.h`.

## Memory traps (these have all bitten before — preserve fixes)

- **GT911 returns big-endian coordinates on this panel** (contrary to most
  GT911 references). Decode each 16-bit field as
  `((uint16_t)hi << 8) | lo` with explicit ordered reads — never
  `Wire.read() | (Wire.read() << 8)` (unspecified evaluation order).
- **Never value-initialize a large struct into a stack temporary**
  (`out = Big{}`): a 34 KB temporary once overflowed the 8 KB Arduino main
  stack and boot-looped the device. `memset` in place.
- **Large resident state goes in PSRAM**, never `static` in internal
  `.bss`: a 34 KB static config struct in internal SRAM starved NimBLE and
  the controller hung silently between WiFi AP and BLE init. The MIDL screen
  arenas, FreeformState and the record store follow this rule.
- **`NimBLECharacteristic::setValue` overload trap**: always use the
  `(const uint8_t *data, size_t len)` form. `setValue(const char *)` can
  resolve to `setValue(uint32_t)` and store 4 bytes of pointer.
- **BLE attribute values are capped at 512 bytes.** The CONFIGURATION
  characteristic is a read-only MIDL status summary; records (48 KB cap)
  come in over HTTP (`PUT /api/midl/records/<id>`) or the manager pull.
- **Never build a large struct on a task callback's stack.** The NimBLE
  host task (~4 KB stack), the WebServer task, and even the Arduino
  loopTask (8 KB) are small. A `proto::AttachAck`/`DeviceRecord` (~1.5 KB)
  or `proto::ControlState` (~2 KB) declared as a stack local inside a GATT
  `onRead`/`onWrite`, an HTTP handler, or a controller loop overflows the
  stack and reboots the device with **no panic text** (the crash precedes
  console init: ROM boot → `entry` → `RTC_SW_SYS_RST`, repeating). Make
  such scratch structs function-static (GATT/web callbacks run serially
  on one task, so a function-static is race-free) + `memset` in place.
  Anything over ~1 KB goes in PSRAM, not internal `.bss`:
  `static T &x = *yeyboats::psram_new<T>();` (`include/psram_static.h`).
  Plain `static` is only for small scratch. Internal static DRAM eats the
  WiFi RX headroom (OTA failed mid-transfer, instruments#41), and
  `make dram-check` / CI fail when `.dram0.data + .dram0.bss` exceeds the
  budget in `tools/check_dram_budget.py`. This is the 34 KB-temporary trap above in a new
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
- **`ui_markers::draw_glyph` allocates a per-glyph PSRAM canvas buffer; LVGL
  does not own a user-supplied canvas buffer.** Since live theme switching
  made screens rebuildable, every such buffer (and the other heap states on
  rebuildable roots: FreeformState, composite HUD states, hull point arrays) is freed via an `LV_EVENT_DELETE` callback registered at
  allocation time — both teardown paths (`reset_screens`, `replace_screen`)
  go through `lv_obj_delete`, which fires DELETE on the whole subtree. If you
  allocate a buffer an LVGL object points at, register the same DELETE-free
  callback or it leaks PSRAM on every theme switch / re-apply. Marker rings
  build glyph canvases at screen-build time; `marker_ring_update` never
  allocates. Keep glyph drawing on the UI task.

## Adding things

### A new SignalK path
1. Field on `sk::Data` in `include/signalk_parser.h`.
2. Path → field branch in `applyValue` in `src/signalk_parser.cpp`.
3. Test case in `test/test_parser/test_parser.cpp` (host-runnable).
4. `subscribe()` entry in `src/signalk.cpp` so the server sends it.
5. To show it on screen, bind it in a MIDL record. A path without a typed
   `MetricSource` (`ui::metric_map::path_to_source`, `src/metric_source_map.cpp`)
   still renders through the dynamic PathStore; add a typed source only for
   the fast path or special formatting.

### A new built-in screen
Add or edit the record in the midl repo (`midl/library/builtin/*.midl.json`,
with a `variants` entry per display class), bump the `midl` submodule, run
`make embed-records`, and commit the regenerated
`include/midl_gen/<class>/builtin_records.h`. Never add a C++ content screen.

### A new board variant
1. `include/board_pins_<board>.h` with full GPIO map.
2. `[env:<board>]` block in `platformio.ini` with a `-D BOARD_<NAME>` flag.
3. Switch on the macro in `main.cpp` to include the right pins file.
4. Add a CI matrix entry in `.github/workflows/ci.yml`.

### A new console command
Pick the right handler: `net` (connectivity/identity), `sk` (SignalK target),
`manager`, or `handleMainCommand` in `main.cpp` (UI actions like `screen`,
`demo`, `theme`). It will automatically be reachable from
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
  the device via `python3 tools/yeydisp.py recover`) should happen
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
firmware-specific contracts (memory traps, dispatch funnel, PSRAM rule,
MIDL-only screens) in this file win where the two overlap.

## Conventions

- **Commits**: Conventional Commits (`feat:`, `fix:`, `refactor:`, `docs:`,
  `chore:`, `test:`) — used by the `release.yml` workflow to generate
  release notes from a tag.
- **Code style**: C++17, LLVM-ish, 4-space indent, brace on same line.
  `make format` runs clang-format in place. Run `make pre-commit` before
  committing; CI and `.githooks/pre-commit` use the same target, so do not
  skip lint locally and leave CI to fail on formatting.
- **Licensing/identity**: project is attributed to **"Yey Boats Project"** —
  do not insert real names in headers, license, or README. License is
  **PolyForm Noncommercial 1.0.0**; do not relicense or add an OSI-OSS
  dual-license without explicit instruction. Commercial licensing contact:
  **rights@yey.boats** (see COMMERCIAL.md).
- **Releases**: `make release-tag VERSION=v0.x.y` then `git push origin
  v0.x.y`. Tags matching `*-rc*`/`*-alpha*`/`*-beta*` are auto-marked
  pre-release.

## Demo / SignalK auth note

The SignalK manager plugin and its deployment (test-server config, Docker
compose, `run.sh`/`run-remote.sh`/`stop*.sh`) now live in
**yey-boats/Instruments-manager** — clone it next to this repo as
`../signalk-espdisp-manager` (or set `MANAGER_DIR`). The `make demo-*`
targets call its `deploy/scripts/`.

`yey-boats-sim` (the full-blown simulator from `yey-boats/simulator`, invoked via
the `yey-boats-sim` CLI) and the firmware both authenticate against SignalK
with a token (`?token=` on the WebSocket). The bundled `signalk/signalk-server`
Docker image rejects anonymous writes by default — first run creates the
admin user (the demo scripts assume username `admin`, password `admin`).
Configure the simulator with env vars `SIGNALK_HOST` / `SIGNALK_PORT` /
`SIGNALK_USERNAME` / `SIGNALK_PASSWORD` (defaults: localhost/3000/admin/admin)
or flags `--signalk-host` / `--signalk-port`. In the lab it runs as the
`kdcube-sim.service` systemd unit; see `docs/sim/kdcube-simulator.md`.

## Recovery notes

- Bad WiFi credentials or a runaway OTA can wedge the device; recovery is
  `esptool erase_region 0x9000 0x5000` (NVS) over USB, then re-provision.
- The CH340 USB-UART is flaky; cycle the cable before assuming a
  firmware/serial problem.
- iOS Personal Hotspot blocks mDNS — use the raw IP for `make ota`.
