# YB-MIDL Full Screen Cutover — Roadmap

> Goal: re-implement the firmware's full legacy screen stack as MIDL documents, loadable from flash per the original design (factory-default in firmware, current/last-known-good tiers). Extend MIDL where a legacy design isn't addressable. Executed screen-by-screen with subagents + review.

Source: two screen audits (2026-06-22). Branch `feat/midl-firmware-render`.

## Screen inventory & verdicts

| Screen | id | Verdict | Needs |
|---|---|---|---|
| Dashboard | dash | EXPRESSIBLE | windrose+SOG+DEPTH+batt-bar (proven on device) |
| Demo grid | demo_grid | EXPRESSIBLE | 4× single-value + tap-nav |
| Navigation | nav | EXPRESSIBLE | compass(HDG, CTS marker)+SOG+COG+position(text) |
| Steering | steering | EXPRESSIBLE* | compass(HDG/CTS)+XTE+VMG+RUDDER + nudge buttons (actions) |
| Route | route | EXPRESSIBLE* | DTW+BTW(/CTS)+XTE+VMG grid |
| Depth | depth | EXTENSION | hero size/scale attr + **trend** sparkline (water-temp/SOG/TWA stats) |
| Status | status | EXTENSION | **local/device bindings** (wifi/ip/rssi/heap/psram/uptime/build) + 13-row **list** (>4 tiles) |
| Trip | trip | EXTENSION | **computed/accumulated** bindings (distance/time/avg/max) — firmware virtual paths + trip-reset |
| Autopilot | autopilot | EXTENSION | composite **HUD**: marker-ring(HDG/COG/CTS/AP-tgt)+center readouts+dial tap-zones + **xte-strip** + AP **mode control** |
| Wind | wind | EXTENSION | fullscreen dial: rotating bezel+upright cardinals, close-hauled arcs, tide vector, A/T markers, responsive |
| Wind classic | wind_classic | EXTENSION | as wind + interior hero readouts + corner boxes |
| Wind steer | wind_steer | EXTENSION | semicircle compass + wind sectors (adaptive) + xte-strip + tiles + course buttons + computed TWD |

\* needs button **actions** + compass **secondary markers** (shared foundation).

## Required extensions (grouped)

**Foundation (blocks everything):**
- F1. **Multi-screen `apply_doc`**: register ALL screens in a doc, wire navigation (next/prev/by-id). Today renders one screen.
- F2. **Raise `MAX_TILES_PER_SCREEN`** (POD growth) + bump `square-480` `maxTiles` in catalog/manifest. Target 8 (steering+buttons, autopilot, status). Re-run POD-size guard.
- F3. **Flash loading (LittleFS)** per spec §3.7: bake the full screen doc as **factory default** in firmware; persist current/last-known-good; load on boot; reset API. (Slice 3.)

**Element/binding extensions:**
- E1. **Button actions**: `action {kind: nav|put|command, target, value/delta}` executed on tap via `net::dispatchCommand`/PUT. Needed by steering/route/wind_steer/autopilot. (No gating — per prior decision.)
- E2. **Compass secondary markers**: render CTS/COG markers on a `compass` from `dir`/`markers[]`. Verify current painter; extend if needed.
- E3. **`trend`** element: real sparkline (ring buffer, sample period, auto-range). (WidgetKind::Trend is a stub today.)
- E4. **Element size/scale attr**: hero 2× (depth). Map `format.size`/`style.size`.
- E5. **Local/device source kind**: `{kind: local, id: device.ip|wifi.ssid|device.heap|...}` → device state resolver. (status)
- E6. **Computed/accumulated source kind**: trip distance/time/avg/max — expose as firmware virtual paths or a `computed` kind. (trip)
- E7. **Composite HUD elements** (catalog additions + painters): `steering-hud` (marker-ring+center readouts+tap-zones), `xte-strip`, `autopilot-control`, and a fullscreen **wind-dial** (bezel+cardinals+arcs+sectors+tide). These are the big ones — port `ui_compass.cpp`/`screen_wind*.cpp`/`screen_autopilot.cpp` drawing into reusable MIDL elements.
- E8. **`status-list`** element OR allow >maxTiles via a list element for the 13-row status screen.

Each new catalog element must be added in `midl/cpp/include/yb_midl_catalog.h` (→ manifest regen + `dispatch-covers-catalog` guard), schemas, and the firmware painter — keeping the MIDL-core dep-free invariant.

## Execution order

1. **F1 multi-screen + nav** → render a multi-screen doc; navigation works.
2. **F2 maxTiles bump** → fit richer screens.
3. **Expressible batch**: dash, demo_grid, nav, route (+E2 compass markers). Validate each on device.
4. **E1 button actions** → steering (nudge), then steering screen.
5. **F3 flash loading** → bake the doc-so-far as factory default; load on boot; `YEYBOATS_MIDL_ONLY` boots the flash doc (not just the baked demo).
6. **Extension screens** one by one, each adding its extension:
   - depth (E3 trend, E4 size)
   - status (E5 local bindings, E8 list)
   - trip (E6 computed)
   - autopilot (E7 HUD composites + E1 actions)
   - wind_steer, wind, wind_classic (E7 wind-dial composites)
7. **Validate all 9 catalog elements** render correctly across the screen set.
8. **Cutover**: default build boots the full MIDL screen set from flash; retire legacy `screen_*.cpp` where MIDL covers them (keep config screens native).

## Invariants (carry from prior work)
- MIDL core (`midl/cpp`, validators) stays **zero external deps**; firmware uses ArduinoJson behind it.
- `apply_doc` selection logic needs **host tests** (the array/element-lookup bugs proved this gap) — extract a pure helper + test as part of F1.
- Memory traps: PSRAM doc/arena, no large stack temporaries, LVGL only on UI task, `memset` not `=Config{}`.
- Every device claim **screenshot-verified** on the bench (lab device via relay; auth bypass build for dev per user).

## Validation per screen
Each screen: build → flash (MIDL-only or via `midl-render`/flash-load) → `screen <id>` → `/api/screenshot.png` → compare to the legacy screen's known render. No screen is "done" without a device screenshot.
