# Screen audit — device ↔ manager (2026-06-19)

Full pass over every data-bearing screen: (1) value presentation with
representative values, (2) device↔manager visual equivalence, (3) design /
implementation glitches, (4) manager config usability + does pushed config
apply on the device. Seven parallel read-only audits; per-screen reports were
captured under `/tmp/screen-audit/`. This file records the findings, what was
fixed in this pass, and what is deferred (needs a design decision).

## Screen inventory

| Screen | Device file | Manager renderer | Notes |
|---|---|---|---|
| dashboard (quadrants) | `screen_dashboard.cpp` | generic grid + `compassTileSVG` | editable heart |
| nav | `screen_nav.cpp` | grid | SOG/COG/HDG/pos |
| depth | `screen_depth.cpp` | grid | depth / water temp |
| steering | `screen_steering.cpp` | grid | HDG bug / CTS / XTE |
| route | `screen_route.cpp` | grid | DTW/BTW/CTS/XTE/VMG |
| trip | `screen_trip.cpp` | grid (no odometer) | bespoke NVS odometer on device |
| status/system | `screen_status.cpp` | `systemPanel` | diagnostics list |
| wind | `screen_wind.cpp` | `windDial` | redesigned dial |
| wind_classic | `screen_wind_classic.cpp` | `windDial` (shared) | comparison-only |
| wind_steer | `screen_wind_steer.cpp` | `windSteer` | sailing aid |
| autopilot | `screen_autopilot.cpp` | `autopilotHud` | glass-cockpit HUD |
| zoom | `ui::zoom` (`ui_layouts.cpp`) | n/a | fullscreen single value |
| knob ap_hud/compass/wind/big | `knob_ui.cpp` | n/a | round 360 form factor |

## Fixed in this pass (verified: 297 native tests, production build, clang-format, manager tests)

### Device firmware
1. **belowKeel rendered belowTransducer.** `path_to_source` mapped
   `environment.depth.belowKeel` → `Depth_m`. A manager-pushed "below keel"
   tile showed the transducer depth. → `DepthKeel_m`. (`layout_renderer.cpp`)
2. **CTS path was unmapped.** `navigation.courseRhumbline.bearingTrackTrue`
   had no `path_to_source` entry, so pushed CTS tiles resolved to nothing.
   → added → `CTS_deg`. (`layout_renderer.cpp`)
3. **Editor-pushed tiles were unitless.** The renderer hardcoded `mb.unit = ""`,
   so every pushed SOG/DEPTH/BATT/… tile rendered with no unit suffix. Added
   `unit_for_source()` deriving the native unit (kn/°/m/°C/V/nm) from the bound
   source; sources whose value already embeds a qualifier (AWA/TWA side letter,
   SOC %, XTE side, position, AP state) stay blank to avoid double-printing.
   (`layout_renderer.cpp`)
4. **Zoom hero number clipped / off-center.** `transform_scale(410)` had no
   `transform_pivot`, so it scaled from the (0,0) top-left default — pushing the
   big digits down-right of CENTER and clipping their top. Added center pivot.
   Verified in re-rendered `zoom-num`/`zoom-pos`. (`ui_layouts.cpp`)
5. **Device never reported its SSID** to the manager heartbeat, so the System
   panel's SSID row was permanently "--". Added `net::ssidString()` and
   `network.ssid` to the telemetry object. (`net.cpp`, `net.h`, `manager.cpp`)

### Manager webapp
6. **Route metrics rendered as degrees (XTE 23 m → "1318°").** Both classifiers
   matched the `course`/`track` substring in `navigation.courseRhumbline.*`
   before length/speed. Rewrote `unitFor` (live-preview) and `quantityForPath`
   (field-schema) to classify by the **leaf segment**, with length leaves
   (`crossTrackError`→m, `distance`→nm) and speed (`velocityMadeGood`→kn) taking
   precedence. Added a `length: ['nm','m','ft']` unit family + `nm` conversion in
   the preview. (`public/live-preview.js`, `lib/field-schema.js`)
7. **Autopilot HUD froze the mode badge.** It printed literal "STBY"/"AUTO"
   regardless of live state. Now mirrors the device: chip ON/STBY by engagement,
   center badge = live AP state (green engaged / dim disengaged), OFFLINE when no
   AP data. Same fix applied to the wind-steer top bar. (`public/device-hud.js`)
8. **Compass tile hardcoded the secondary line to COG.** It ignored
   `tile.secondary`, so the steering screen (bound to CTS) still showed "COG".
   Now resolves the secondary path to its label (COG/CTS/BTW/HDG/TWD/TGT).
   (`public/device-hud.js`)
9. **RSSI "0 dBm" in AP mode.** The device returns `rssi()==0` as a down
   sentinel; the panel printed "0 dBm". Now shows "--". (`public/device-hud.js`)

## Deferred — need a design decision (documented, not changed)

- **Editor→device tile schema mismatch (biggest gap).** The editor authors
  `{widget:<slug>, path, unit}` with view types `windCircle`/`control`; the
  device parser reads `{widget:<kind>, primary_path, secondary_path}` and knows
  `windRose`/`autopilot`. Editor-authored (non-preset) tiles fall back to a blank
  Numeric on the panel. Needs a flatten/alias layer (manager-side on push, or
  device-side in `layout.cpp`).
- **Unit conversion engine.** The editor offers ft/°F/m·s⁻¹; the device renders
  fixed SI-derived units. Fix #3 carries the *native* unit; honoring a chosen
  non-native unit needs conversion + a `layout::Tile.unit` field.
- **Fullscreen HUDs presented as editable.** autopilot / wind / wind_steer /
  status are built-ins that ignore pushed path config (silent no-op), yet the
  editor shows a field form / presets for them. Gate the editor on the device
  capability manifest and mark them read-only.
- **HUD marker rings.** Device autopilot/wind_steer draw HDG/COG/CTS/target glyph
  rings; the manager HUDs draw only the amber target/TWD bug.
- **Compass tile model.** Device tile is north-up with moving marker glyphs;
  manager `compassTileSVG` is heading-up with rotating cardinals and no markers.
- **TWD derivation divergence.** Device computes TWD = heading + `angleTrueWater`
  (never parses `directionTrue`); the manager binds TWD → `directionTrue`. They
  diverge under current/leeway.
- **Tile-count truncation.** Manager wide/xwide presets author 6/8 tiles; device
  QuadGrid is hardwired to 4 (`MAX_TILES_PER_SCREEN`) and silently drops the rest.
- **TRIP has no manager renderer.** Device is a bespoke NVS odometer; the manager
  shows a generic SOG/STW/CURRENT grid. Pushing a manager `trip` layout would
  `replace_screen` and destroy the odometer.
- **wind_classic** shares `windDial` (the *redesigned* renderer), so its preview
  matches the wrong screen; it also re-introduces the label/marker overlap the
  redesign fixed. Either give it a faithful renderer + cleanup or retire it.
- **Harness gap:** no `status`/`system` headless render — add it to `sim_render.sh`.
- **Cosmetic:** AWA/TWA tiles show `42S` (no degree sign); bearings not always
  zero-padded in the manager; "--" vs "---" placeholder inconsistency.
