# YB-MIDL Migration — Next Session Task Definition

> Handoff for continuing the MIDL native-rendering migration. The foundation
> (delivery, multi-screen, nav, 7/9 elements, idf5 double-buffer) is merged to
> `main`. This defines the remaining work, the validation workflow, and the
> traps that have already bitten — so the next session starts productive.

## Where things stand (merged to `main` from `feat/midl-firmware-render`)
- **MIDL renderer**: multi-screen `apply_all` (per-screen PSRAM arena pool +
  template trampolines), navigation, atomic `reset_screens` on re-apply.
- **Delivery**: `POST/GET /api/midl/config` + `POST /api/midl/reset?to=default|previous`
  (in-RAM current/previous doc store). This is the endpoint the manager pushes to.
- **Elements rendering on hardware (7/9)**: single-value, text (incl. position),
  bar, compass (cardinals + marker), windrose, autopilot (state pill + nudge
  buttons), button. Gauge **value** binds; gauge **range display** + **trend** pending.
- **Attr plumbing**: `MetricBinding` has `range_min/range_max/precision`;
  `map_element` reads `format.range`/`format.precision`; rudder path bound.
- **idf5 double-buffer**: `num_fbs=2` DIRECT on the esp_lcd RGB panel +
  mbedTLS-PSK link fix — integrated; all envs build. This is the perf substrate.
- **Flags**: `YEYBOATS_MIDL_ONLY` (boot the baked/flash MIDL doc only),
  `YEYBOATS_LAB_OPEN_WEB` (lab-only web-auth bypass — never ship).
- Pure host-tested `select_screen`/`find_element` (the apply_doc selection that
  had 2 bugs is now covered). Host suite: 346 tests.

## Validation workflow (use this — it's fast)
- **Lab device** via relay: `ssh compulab@mythra-nav` (key auth) → device at
  `10.42.0.67` on the relay AP. OTA through the relay:
  `PLATFORMIO_BUILD_FLAGS="-D YEYBOATS_MIDL_ONLY=1 -D YEYBOATS_LAB_OPEN_WEB=1" make ota-verify DEVICE_IP=10.42.0.67 REMOTE=compulab@mythra-nav`
- **Live-push docs without reflashing** (the big speedup):
  `ssh compulab@mythra-nav 'curl -s -X POST -H "Content-Type: application/json" --data-binary @/tmp/doc.json http://10.42.0.67/api/midl/config'`
  → `{"ok":true,"screens":N}`. **MUST be `application/json`** (form-urlencoded → empty body).
- **Screenshot**: `ssh ... 'curl -s http://10.42.0.67/api/screenshot.png' > shot.png` then Read it.
- **GOTCHA — async `/api/cmd`**: `screen <id|N>` commands queue with lag and race
  the screenshot. Trust the `[ui] show(N)... loaded active=<ptr>` diagnostic logs
  (`/api/logs`) over `/api/state` timing. For a clean single-screen check, push a
  one-screen doc and screenshot directly (no nav).
- **Re-secure when done**: reflash a default build (no flags) → `/api/*` returns 401.

## Tasks (priority order)

### T1 — Gauge range display (painter)
Gauge **value** binds, but `format.range:[-35,35]` doesn't show: tick labels are
hardcoded 0/25/50/75/100 at build time and the center shows "100%" (percent path).
Fix `paint_gauge_body` (build) + the update path in `src/ui/ui_layouts.cpp` to
drive the **tick labels** and **center scalar** from `m.range_min/range_max`/
`m.precision` when set; keep the legacy 0–100/percent fallback when unset
(`range_min==range_max`). Validate: push a full-screen rudder gauge, confirm
ticks read −35…35 and center shows the live angle.

### T2 — `trend` element (sparkline)
`WidgetKind::Trend` falls back to numeric today. Implement a real rolling
sparkline (ring buffer + auto-range) per `screen_depth.cpp`'s chart. Validate on
a `trend`-bound depth tile.

### T3 — Button actions (E1)
Buttons/autopilot render but don't ACT. Wire `element.action {kind: nav|put|command,
target, value/delta}` → on tap, `net::dispatchCommand` / SignalK PUT (nudge ±1/±10,
tack, AP mode). No gating (per prior decision). Validate steering nudge on hardware.

### T4 — Remaining simple screens as MIDL docs
nav, dash, demo_grid, speed already work; add **route** (4 numeric tiles) and
**steering** (compass + XTE/VMG/RUDDER + nudge buttons, needs T3 + raised maxTiles).
**T4a — raise `maxTiles`** (POD growth `MAX_TILES_PER_SCREEN`→8 + manifest bump +
re-run the POD-size guard) so steering+buttons / status fit.

### T5 — Extension bindings/elements
- **status**: local/device bindings (`{kind:local, id:device.ip|wifi.ssid|device.heap|...}`)
  via a device-state resolver, + a `status-list` element (13 rows > maxTiles).
- **trip**: computed/accumulated metrics (distance/time/avg/max) — expose as
  firmware virtual paths or a `computed` source kind.
- **depth**: hero size/scale attr (`format.size`) + T2 trend.

### T6 — HUD / wind-dial composite elements (the big lift)
Port `ui_compass.cpp` / `screen_wind*.cpp` / `screen_autopilot.cpp` drawing into
reusable MIDL catalog elements: `steering-hud` (marker-ring HDG/COG/CTS/AP-target +
center readouts + dial tap-zones), `xte-strip`, and a fullscreen `wind-dial`
(rotating bezel + upright cardinals + close-hauled arcs + wind sectors + tide
vector). Each new element: add to `midl/cpp/include/yb_midl_catalog.h` (→ manifest
regen + `dispatch-covers-catalog` guard) + schema + firmware painter, keeping
MIDL-core dependency-free. Covers wind, wind_classic, wind_steer, autopilot.

### T7 — Flash persistence (A2, LittleFS)
LittleFS partition (partitions.csv change — verify OTA/app fit). `midl_store`:
factory (rodata) / current / last-known-good. Boot loads `current` (else factory).
A/B: promote current→last-good after clean render; boot-loop counter (NVS) reverts.
`YEYBOATS_MIDL_ONLY` then boots the **flash** doc, not the baked demo.

### T8 — Manager MIDL push (Phase B, `../signalk-espdisp-manager`)
Per `2026-06-19-yb-midl-manager-migration.md` Slice 5: manager validates a doc vs
the device's `/api/midl/manifest`, resolves+migrates to the device class/version,
and `POST`s it to `/api/midl/config` with the `X-YeyBoats-Authorization` device
token (device must accept that token on the POST — currently web-auth only).
Restores the manager↔device link on MIDL. See `docs/midl/manager-device-sync.md`.

### T9 — Cutover (D)
Default build boots the full MIDL screen set from flash; retire legacy
`screen_*.cpp` where MIDL covers them (config screens stay native); soak +
screenshot parity vs legacy renders; remove the manager v2 path.

## Traps already paid for (don't re-learn these)
- **gnu++11 aggregate init**: base `esp32-4848s040` compiles app TUs as gnu++11.
  Default member initializers on a POD (e.g. `MetricBinding`) make it a
  non-aggregate → breaks every legacy positional brace-init table. Add fields
  TRAILING, value-init to 0, no `= x` initializers.
- **Memory traps (CLAUDE.md)**: live config/arena/doc in PSRAM (never static
  SRAM — NimBLE starvation); `memset(&x,0,sizeof x)` not `x = T{}` (34 KB stack
  temp boot-loops); no large structs on web/GATT/task-callback stacks; all
  `lv_obj_*` on the UI task; `apply_all`/screen_manager run on the UI task.
- **apply_doc selection bugs (fixed, keep host-tested)**: MIDL `screens` is a JSON
  ARRAY (not object); element lookup by explicit `strcmp` (ArduinoJson `operator[]`
  pointer-identity misses the solver's key buffer). Covered by `test_midl_select`.
- **Screen re-apply** must atomically replace the set (`reset_screens` parks on a
  blank root, deletes old eager roots, resets count) — and `show()` must ignore
  its early-return while parked, or nav sticks on screen 0.
- **clang-format**: local v22 flags `main.cpp:262-263` (idf5 merge, CI-version
  passed). Don't reformat — it diverges from CI's pinned version.

## Key references
- Plans: `2026-06-22-yb-midl-full-migration.md`, `2026-06-22-yb-midl-screen-cutover-roadmap.md`
- Spec: `2026-06-19-generic-dashboard-runtime-design.md` (§5 scope)
- Sync: `docs/midl/manager-device-sync.md`
- MIDL submodule: `midl/` (catalog `cpp/include/yb_midl_catalog.h`, schemas, TS validator) — keep core dependency-free.
- Element audit (per-screen field/path/layout specs): the two audit reports summarized in the cutover roadmap.
