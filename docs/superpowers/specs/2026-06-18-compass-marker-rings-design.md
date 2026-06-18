# Configurable marker rings for compass-like widgets

**Status:** Design · **Date:** 2026-06-18 · **Author:** navado and contributors

## Problem

The firmware's compass-like widgets each carry **one** hard-wired direction
marker:

- the **Steering tile** Compass (`ui_layouts.cpp` `paint_compass_body`) draws a
  single accent triangle orbiting the ring plus a CTS text line;
- the **Autopilot HUD** (`ui_compass.cpp` / `screen_autopilot.cpp`) draws a fixed
  red lubber and one amber AP-target bug; COG appears only as sub-line text;
- the **wind rose** tile (`paint_wind_rose_body`) and the **wind / wind-steer**
  screens carry their own one-off apparent/true heads.

Operators want to read **heading (HDG), course over ground (COG), and course to
steer (CTS)** at a glance on the steering views — three distinct markers on one
dial — and the device-mirrored layout editor (see the umbrella spec,
`2026-06-17-device-mirrored-layout-editor-design.md`) needs a way to *configure*
those markers per field, not just the single `dir` the compass field model
currently allows. The marker concept generalizes: a compass-like widget is a
**center number plus a list of markers on a circle**, and a marker can be bound
to any angle-typed path — including the bearing to an AIS or radar target, a
landmark or recorded layline, not just own-ship HDG/COG/CTS. The steering
defaults are three (HDG/COG/CTS), but the list is bounded only by the manifest
cap (default 12), so target- and layline-rich dials fit too.

This design adds a shared marker primitive to the firmware, models the marker set
as an abstract configurable list, and keeps the manager's preview renderers
faithful to the device.

## Goals

1. **Abstract marker list** — a compass-like widget renders an ordered list of
   markers (up to the manifest cap `maxMarkersPerDial`, default 12), each
   `{ id, glyph, filled, color, source/path }`, independent of the center value.
2. **Reference-relative placement** — every marker sits at
   `screen_angle = marker.value − reference`, where `reference` defaults to the
   center value (so the dial is "value-up"); a marker equal to the reference lands
   at the top (offset 0).
3. **One glyph source of truth** — a shared `ui_markers` module owns the glyph
   set and placement math; every compass-like renderer consumes it (no
   per-renderer duplication, no inline colors/sizes).
4. **Configurable per the editor** — the field schema and capability manifest
   carry the marker list; the editor gates a marker's path to **angle-typed**
   paths and offers the device's glyph set.
5. **Device-mirrored previews** — the manager's preview/live renderers implement
   the identical marker model and glyph set, so an authored marker previews
   exactly as the device draws it.

### Non-goals

- Reflowing the firmware-owned HUD geometry (autopilot / wind / wind-steer layout
  stays firmware-owned; the editor previews it, per the umbrella spec non-goals).
- Computing AIS/radar target bearings in firmware. Markers render whatever
  **angle-typed path** is bound; deriving a bearing-to-target path is a SignalK
  source/server concern. The firmware just resolves the path via the generic
  value store and places the marker.

## Marker model

A compass-like widget = an optional **center value** + a **reference** + a
**marker list**.

```jsonc
{
  "type": "compass",
  "title": "HDG",
  "value":  { "path": "navigation.headingTrue" },   // center number (any scalar)
  "reference": "value",                               // "value" | "north" | <angle path>
  "markers": [                                        // ordered, <= maxMarkersPerDial (default 12)
    { "id": 1, "glyph": "triangle", "filled": true,  "color": "accent",
      "path": "navigation.headingTrue" },
    { "id": 2, "glyph": "triangle", "filled": false, "color": "good",
      "path": "navigation.courseOverGroundTrue" },
    { "id": 3, "glyph": "diamond",  "filled": true,  "color": "warn",
      "path": "navigation.courseRhumbline.bearingTrackTrue" }
  ]
}
```

### Center value (separate binding)

The center number is its own binding, default `navigation.headingTrue` (HDG), and
may be any scalar with its own unit/format (e.g. AWS on the wind instrument). It
is **not** required to correspond to any marker. A marker for HDG is just one
entry in the list — so on the autopilot HUD (a rotating-scale, heading-up dial)
HDG shows both as the big number and, with `reference = value`, as the offset-0
marker at top under the lubber. The round Compass tile keeps a static bezel, so
it is north-up (HDG is the big number and a marker at its true bearing); see the
per-widget note under Reference.

### Reference (rotation origin)

`reference` is the subtrahend in the placement formula and selects what sits at
the top of the dial:

- `"value"` (default) — the center value, **when it is an angle**. Reproduces
  heading-up automatically on dials whose **scale rotates** (the autopilot HUD,
  where the tick ring + labels turn by −heading so HDG rides under the lubber).
- `"north"` — fixed 0°, i.e. north-up. The right default for **fixed-bezel**
  dials whose cardinals do not rotate (the round Compass tile keeps a static
  N/E/S/W bezel, so its markers sit at their true bearings to match it). Frame is
  therefore **per-widget**: rotating-scale dials default to value-up, fixed-bezel
  dials to north-up.
- `<angle path>` — any angle-typed path (e.g. bind reference to HDG while the
  center value shows a non-angle like AWS).

When the center value is **not** an angle (wind instrument center = AWS, a
speed), `reference = "value"` is invalid (cannot subtract a speed from a bearing);
it falls back to `"north"` unless an explicit reference angle is configured. The
editor enforces this: if the center value's path is non-angle, the reference
selector excludes `"value"`.

### Marker

```
Marker {
  uint8_t  id;          // ordinal; lets two markers share a glyph but differ by id/color
  Glyph    glyph;
  bool     filled;      // hollow (outline) vs filled
  uint32_t color;       // resolved from a theme token (no inline magic)
  <source>;             // MetricSource (built-in screens) | path string (authored fields)
}
```

`<source>` is a `MetricSource` for built-in screens (fast-path, e.g. `HDG_deg`,
`COG_deg`, `CTS_deg`, `BTW_deg`) and a **path string** for editor-authored fields
(resolved via the generic path→value store from slice 1 of the umbrella spec).
Glyphs may repeat across markers with differing id/color — permitted; the editor
flags duplicate glyphs as not-advisable rather than blocking them.

### Placement (shared contract)

Each refresh, per marker:

```
screen_angle = wrap360( marker.value − reference )
rotate marker holder to (screen_angle * 10)   // LVGL 0.1deg units
hide marker if screen_angle falls in the renderer's occluded window
```

- `wrap360` normalizes to `[0, 360)`.
- A marker whose value equals the reference → `screen_angle = 0` → top.
- The **occluded window** is renderer-specific: the AP semicircular compass
  occludes its lower half (reusing the existing degree-label hide logic at
  `|rel| > 96°`); full-circle dials never occlude.

This contract is identical in firmware and the manager preview renderer.

## Glyph set

The shared glyph set, each available **hollow (outline)** and **filled**:

| Glyph         | Token           | Notes                                  |
|---------------|-----------------|----------------------------------------|
| Triangle      | `triangle`      | default heading/marker head            |
| Diamond/romb  | `diamond`       | CTS default                            |
| Circle        | `circle`        |                                        |
| Bar / tick    | `bar`           | thin radial `\|`                        |
| Cross         | `cross`         | `+`                                    |
| Chevron in    | `chevron_in`    | wind "heart-with-angle", points inward |
| Chevron out   | `chevron_out`   | wind head, points outward              |
| Chevron left  | `chevron_left`  | `<`                                    |
| Chevron right | `chevron_right` | `>`                                    |
| Chevron double| `chevron_double`| `<>` opposing                          |

Glyphs are drawn with LVGL draw primitives (lines / canvas), colored from a
passed theme token — no font-symbol dependency, no inline color literals. The
firmware enum and the manager's glyph table use the same token names.

## Firmware design

### Shared module (`include/ui_markers.h` + `src/ui/ui_markers.cpp`)

Single source of truth for the glyph set and placement:

- `enum class Glyph : uint8_t { Triangle, Diamond, Circle, Bar, Cross,
  ChevronIn, ChevronOut, ChevronLeft, ChevronRight, ChevronDouble };`
- `lv_obj_t *draw_glyph(lv_obj_t *parent, Glyph g, bool filled, uint32_t color);`
  — builds one marker visual.
- `struct MarkerRing` — owns up to `MAX_MARKERS` (= `maxMarkersPerDial`, 12)
  orbiting holders concentric with a ring of given diameter + center offset, each
  holder pivoting at the ring center so rotation sweeps its glyph around the dial
  pointing inward. Holders are lazily shown/hidden so an N-marker ring costs N
  active objects, not 12.
- `void marker_ring_update(MarkerRing &r, const double *values, uint8_t n,
  double reference_deg, const OcclusionWindow &occ);` — performs the
  `value − reference` placement and occlusion-hide for the active markers,
  reusing `set_rot_if_changed` / `set_hidden_if_changed` so the partial-render
  path stays cold when nothing moved.

All build/refresh runs on the UI/LVGL task (per the "LVGL only on UI task" rule).

### Wiring

- **Steering tile compass** (`ui_layouts.cpp` `paint_compass_body`): replace the
  single `aux2` direction marker with a `MarkerRing`. Default list
  **HDG ▲filled (accent) / COG △hollow (good) / CTS ◆filled (warn)**,
  `reference = north` (the tile keeps its static N/E/S/W bezel, so markers sit at
  true bearings; HDG is still the big center number). The CTS text line stays.
- **Autopilot HUD** (`ui_compass.cpp` build + `screen_autopilot.cpp` refresh):
  the fixed red lubber stays as the top reference indicator; the single amber bug
  becomes a `MarkerRing`. Defaults HDG / COG / CTS, plus the **AP-target** marker
  shown only when a target heading exists (preserves today's behavior).
  `reference = HDG`; occlusion = lower semicircle.
- **Wind rose tile** (`paint_wind_rose_body`) and **wind / wind-steer**: adopt
  `MarkerRing` for the apparent/true heads (`chevron_in` / `chevron_out`) and any
  bearing markers; `reference` = the screen's configured reference angle (default
  north or HDG per screen), since the center value is a speed.

Screens that don't opt in pass an empty or single-marker list — no regression.

### Built-in marker source resolution

Built-in screens build their default marker list from `MetricSource` values
(fast-path, no string lookups). The `MetricSource → degrees` resolver already
exists in `ui_layouts.cpp` (`format_metric` / the COG/CTS/BTW branches); the
marker path reuses it to fetch each marker's bearing in degrees. Authored fields
resolve `marker.path` through the generic value store.

## Config / manifest contract

Extends the umbrella spec's field model and manifest (this design supersedes the
compass `paths:{value, dir?}` shape there):

- **Field schema** (`config.widgets.items[*]` / `layout.screens[*].tiles[*]`) for
  `compass` and `windCircle`: `value` (center), `reference`
  (`"value"|"north"|<angle path>`), and `markers[]` (≤ `maxMarkersPerDial`) of
  `{ id, glyph, filled, color, path }`. The legacy single `dir` path maps to a
  one-entry marker list (back-compat).
- **Capability manifest** (`ui.capabilities`) gains:
  - `glyphs`: the glyph token list above;
  - `maxMarkersPerDial`: `12` (generous fixed bound; steering defaults use 3);
  - marker `path` gating rule: **angle-typed only** (SignalK `meta.units == "rad"`
    or an angle quantity). The editor offers only angle paths for markers.
- **Documented marker use cases** (why markers are an abstract path list, not
  fixed roles): own-ship HDG / COG / CTS / BTW, target wind angle, **bearing to an
  AIS or radar target**, and **landmark / recorded laylines** — any angle-typed
  path the generic store can resolve (e.g. a derived bearing-to-`vessels.<id>`
  path, a radar-target bearing, or a stored layline bearing). The firmware
  special-cases none of these; it renders the bound angle. Authoring many such
  markers by hand is bounded by `maxMarkersPerDial`; populating them
  automatically from a live collection is the follow-on below.
- The device validates an incoming marker list against its manifest (glyph known,
  count ≤ cap, marker paths angle-typed) and rejects with `ParseError`, falling
  back to the last persisted config.

## Manager preview renderers

The manager (`signalk-espdisp-manager`: widget preview in `screen-presets.js` /
`layout-editor.html`, and the device-mirror render in `device-hud.js`) implements
the **identical** marker model and glyph set in JS/SVG:

- same glyph tokens, same hollow/filled variants, same colors-from-theme;
- same `screen_angle = value − reference` placement and occlusion window per dial
  type;
- the editor's marker rows (add/remove/reorder, ≤3) author the field's `markers[]`
  and gate the path picker to angle-typed paths.

One marker contract, two renderers — captured here, implemented in the manager
slice of the plan.

## Testing

- **Host (native/Unity):**
  - `screen_angle(value, reference)` as a pure function — wrap-around, same-value
    → 0, negative deltas;
  - occlusion-hide predicate for the semicircular window;
  - default marker-list construction per steering screen;
  - config parse/validate against the manifest — glyph-unknown, count-over-cap,
    and non-angle marker path each rejected; legacy single-`dir` maps to one
    marker.
- **Sim:** render the steering compass, AP HUD, wind rose, and wind-steer at
  480 / 800 / 1024 with the HDG/COG/CTS marker lists plus an AIS-bearing marker
  fixture; eyeball glyph/color/placement; regenerate the widget-preview PNGs.
- **Manager (node `test/run.js`):** marker-schema CRUD + config generation;
  angle-path gating; glyph-list gating; back-compat of legacy single-`dir`.
- **Contract:** a shared fixture of a marker-bearing field exercised on both
  sides so the manager never emits a marker config the device would reject.
- **Gate:** `make pre-commit` + `pio test -e native` + `pio run -e esp32-4848s040`.

## Risks / mitigations

- **Glyph drawing cost.** Markers are built once and only rotated/hidden on
  refresh (no per-frame redraw of glyph geometry); `set_rot_if_changed` keeps the
  path cold when nothing moves.
- **Memory.** `MarkerRing` holders are LVGL objects on the UI task; refresh keeps
  the `sk::Data` snapshot-copy pattern — no new large stack temporaries (the
  project's large-struct-on-stack reboot trap).
- **Renderer drift.** Firmware and manager must stay glyph-for-glyph identical;
  the shared contract fixture + regenerated previews catch divergence.
- **Marker crowding.** Converging bearings overlap on a small dial, more so as
  the count climbs toward the cap; hollow/filled + color separation mitigates, and
  the editor flags duplicate glyphs. No auto-declutter in this pass (YAGNI) —
  collision-aware placement is folded into the dynamic-sources follow-on, where
  high marker counts are the norm.

## Future work: dynamic marker sources (out of scope this pass)

Authoring AIS/radar targets and per-mark laylines as individual marker rows does
not scale past a handful. A follow-on design adds a **dynamic marker source** — a
ring binds a live SignalK collection and the firmware rebuilds the active marker
set as members come and go:

```jsonc
"markerSources": [
  { "collection": "ais", "bearing": "<derived bearing path/expr>",
    "glyph": "triangle", "filled": false, "color": "fg_dim",
    "filter": { "rangeNm": 6 }, "max": 12 }
]
```

The ring then renders `markers[]` (static authored) **plus** the resolved members
of each `markerSources[]`. New concerns deferred to that design: collection
subscription + churn/debounce, stable marker **identity/lifecycle** (so a target
keeps its holder across updates), per-source caps and range/sector filtering, and
**collision-aware decluttering** for dense target fields. The static model in
this spec is the substrate it builds on — no rework of the marker/glyph/placement
contract, only an additional source that feeds the same ring.

## Implementation sequencing

1. **`ui_markers` module** — glyph set + `draw_glyph` + `MarkerRing` + placement;
   host tests for the pure placement/occlusion functions.
2. **Steering tile compass** — swap to `MarkerRing` with the HDG/COG/CTS default
   list; sim render.
3. **Autopilot HUD + wind/wind-steer** — adopt `MarkerRing`; preserve the
   AP-target bug and wind heads; sim render at all sizes.
4. **Config/manifest** — extend the field schema + manifest; parse/validate +
   angle-gating; back-compat for legacy `dir`.
5. **Manager preview renderers** — mirror the glyph set + placement; editor marker
   rows + angle-path gating; node tests + shared contract fixture.
