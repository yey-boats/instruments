# SignalK Screens — Preview Fidelity via Device-Reported Bindings

**Status:** Queued (not yet started)
**Date:** 2026-06-18
**Repos:** firmware `espdisp` (this repo) + manager `yey-boats/Instruments-manager` (local `../signalk-espdisp-manager`)
**Source of truth:** the firmware screen renderers — the device display is canonical; the manager preview must mirror it, ideally by rendering the device's *own* reported bindings rather than a hand-maintained copy.

## Problem

The manager's browser layout-editor "Preview" pane must be a faithful mirror of what the
device draws. Today it diverges:

- `assets/2026-06-18-device-vs-preview-wind-steer.jpg` — device renders the full **Wind
  Steer** HUD (semicircle port/stbd band, `AUTO` annunciator, `HDG 167.1°`, `TWA 166°S |
  TWD 333°` subline, four right-rail tiles TWA/TWS/AWA/AWS, STBY/HOME). The browser
  **Preview pane is blank.**
- `assets/2026-06-18-device-vs-preview-depth.jpg` — Depth roughly matches but drifts on
  caption casing (`5m` vs `5M`), spacing, typography.

### Root cause (the important finding)

The device does **not expose its built-in screen tile bindings.** `/api/screens` returns
only `{id, title, hidden}`; `/api/layout` GET returns just the *user-applied* PSRAM layout
(404 when none is loaded). The built-in screens exist only as C++ `MetricBinding`/`QuadGrid`
specs in `src/ui/screen_*.cpp`.

So the manager mirrors the firmware through **two hand-maintained parallel re-descriptions**
that drift on every firmware redesign:

1. `public/device-hud.js` — bespoke SVG for the fullscreen HUDs (autopilot / wind /
   wind_classic / wind_steer / system).
2. `lib/screen-presets.js` — tile presets for the QuadGrid data screens
   (`getPresetsForClass('sunton-480')`).

Wind Steer was rebuilt on the autopilot layout (`f6ad2b1`) and marker rings were merged
(`62e7154`); the manager copy lagged, so its `windSteer` preview now renders blank/stale.
This is a *structural* drift problem, not a one-off bug.

## Goal

Every user-facing device screen renders in the preview **exactly** as on the device, and
stays faithful across future firmware changes — by having the device report its real
bindings instead of the manager guessing them.

## Design

### Part 1 — Firmware: expose built-in screen bindings (this repo)

Serialize each built-in screen's tile bindings into the **same layout JSON shape that
`/api/layout` PUT already consumes** (`screens[].tiles[]` with `widget`, metric/`path`,
`title`, `color`, `zoom`, `markers[]`, plus the screen's template/HUD kind).

- Add a serializer over the `ui::layouts::MetricBinding`/template specs (the capability
  manifest in `src/manager.cpp::capabilities::build_manifest` already walks similar data —
  reuse that path / helpers).
- Surface it so the manager's existing fetch path gets real data: extend **`/api/layout`
  GET to fall back to the serialized built-in layout** when no user layout is applied
  (instead of 404), and/or add `/api/screen/<id>` detail returning one screen's tiles.
- Each screen entry carries a `kind`/`template` discriminator so the manager knows whether
  to render it as a QuadGrid tile grid, a fullscreen HUD, the system panel, or the trip
  stats screen.

**Memory-trap guardrails (this touches `web.cpp` / `manager.cpp`):** build the JSON with
the PSRAM allocator (`espdisp::psram_json`, as `handle_screens` already does); never stack-
allocate the layout/`Config`-sized scratch in the HTTP handler (function-`static` +
`memset`, or PSRAM). Run the `firmware-heap-reviewer` agent on the diff before committing.

### Part 2 — Manager: render from device bindings (manager repo)

- Drive the preview from the device-reported `layout.screens[]` (already the shape
  `live-preview.js` consumes via `cfg.screens`). Make `screen-presets.js` a **last-resort
  fallback** only (e.g. device offline), not the primary source — drop the implicit
  `getPresetsForClass` reliance for connected devices.
- The shared widget vocabulary already exists and largely works (compass+marker / windRose
  / bar / numeric / text in `live-preview.js`, HUD SVG in `device-hud.js`); the win is that
  it now renders the device's *actual* tile bindings, so QuadGrid screens (steering / route
  / nav / dashboard) become faithful without bespoke renderers.
- **Fullscreen HUDs stay hand-written SVG** (they are not tile grids). Give them a one-time
  parity pass against the current firmware (post wind-steer rebuild + marker-ring merge) and
  a **blank-guard**: a HUD renderer that throws or returns empty must degrade to a visible
  labelled stub, never a blank pane (this is the concrete Wind-Steer fix).
- **Trip screen — faithful renderer (in scope):** build a dedicated odometer/stats preview
  renderer in `device-hud.js` mirroring `screen_trip.cpp` (integrated distance + trip stats),
  dispatched like the other fullscreen kinds.

### Part 3 — Lock it with visual tests

Extend the manager's Playwright/visual-test harness so each mirrored screen has a screenshot
baseline; a renderer regressing to blank or diverging from baseline fails CI.

## Device screen inventory & treatment

| Screen(s) | Device renderer | Preview treatment |
|---|---|---|
| autopilot, wind, wind_classic, wind_steer | bespoke fullscreen HUD | hand-written SVG + parity pass + blank-guard |
| steering, route, nav, dashboard, depth | `QuadGrid` tile layout | render device-reported tiles via shared widget painters |
| status / system | diagnostics panel | existing `systemPanel` (faithful) |
| trip | bespoke local odometer/stats | **new** faithful stats renderer |
| touch_cal, touch_grid, wifi_setup, settings, demo_grid, manager | setup/built-in | out of scope |

## Acceptance criteria

- Selecting **any** data screen in the Preview shows a faithful render — never a blank
  pane — for both populated and absent live data.
- The QuadGrid screens (steering/route/nav/dashboard/depth) render from the **device's
  reported bindings**, verified by editing a built-in screen on-device and seeing the
  preview track it (no `screen-presets.js` involvement for a connected device).
- Wind Steer preview matches the device screenshot: semicircle band, AUTO/STBY annunciator,
  HDG center, TWA|TWD subline, four right-rail tiles.
- Trip preview mirrors the device odometer/stats screen.
- Depth (and grid screens) match device caption casing, units, precision.
- Firmware `/api/layout` GET returns serialized built-in bindings when no user layout is
  applied; firmware build + `pio test -e native` pass; `firmware-heap-reviewer` clean.
- A visual-regression baseline exists per mirrored screen in the manager's CI.

## Out of scope

- Changing the device firmware screen *designs* (device is canonical; we only serialize them).
- Setup/built-in screens (touch_cal, touch_grid, wifi_setup, settings, demo_grid, manager).
- The data-simulator enrichment (handled by `yey-boats/simulator` — the full-blown
  `yey-boats-sim` CLI — which makes route/AIS/AP data available to *exercise* these
  previews but is not part of this rework).

## Notes for the implementer

- Two-repo change: firmware serializer + endpoint in `espdisp`; preview sourcing + trip
  renderer + blank-guard + visual tests in `../signalk-espdisp-manager`.
- Cross-check every screen against `src/ui/screen_*.cpp`; use the firmware
  `/api/screenshot.png` endpoint to capture device truth for the visual baselines.
- Prior context: `2026-06-17-device-mirrored-layout-editor-design.md` (manifest/path-store
  slices) and `2026-06-18-compass-marker-rings-design.md` (marker rings already mirrored for
  grid compass tiles) — this rework extends that device-mirroring discipline to the full
  screen set and removes the preset/HUD drift class.
