# MIDL Instrument Design Guidelines

Design conventions for the firmware's MIDL renderer (`src/ui/ui_layouts.cpp`), distilled
from the 2026-06-22 per-screen design review against the canonical web preview
(`yey.boats/instruments`, the `@yey-boats/midl-web` render). These are the rules an
implementer or design agent should follow so screens stay consistent and legible on the
480×480 panel (a 3×3 grid gives ~160 px tiles).

The reference design is the **web preview** (pick a screen + "Square · 480×480"). The same
MIDL document is meant to render recognizably the same on the device glass cockpit, the
SignalK-plugin preview, and the browser. The web preview renders the *library* docs, so
compare at the **element-render** level, not screen-for-screen.

---

## 1. Tile elements (the catalog: single-value, text, gauge, bar, compass, windrose, trend, autopilot, button)

1. **Always show units.** Every value-bearing tile renders its unit. Author it as
   `format.unit` in the MIDL doc; the painter reads `MetricBinding.unit`. A bare number is
   a bug. Units are a small dim suffix (`theme.fg_dim`, one font step down, baseline-aligned
   to the digits) — the pattern in `paint_numeric_body`. **No space before `%`/`°`**; thin
   space (U+2009) before word units (`kn`, `V`, `m`). Ranged gauge/bar labels must append
   `m.unit` too (a `[0,1]`-range SOC bar shows `"77%"`, not `"77"`).

2. **The value is the hero.** The live number dominates (~60% of vertical content). The
   caption sits dim top-left; the unit is subordinate. Fonts scale with tile size via the
   painter ladders and `fit_value_font` / `fit_text_font`.

3. **Never clip a value.** Measure width (`lv_text_get_size`) and shrink to fit. Multi-line
   text (position lat/lon) uses `fit_text_font` (ladder 28/20/14) against `tile_w - 24`.

4. **Tap targets fill the tile.** A `button` element is a large, centered, finger-sized
   rounded bubble filling ~`w-24` × most of the height (radius 14, accent fill via
   `value_color`, dark ink, Montserrat label scaled to bubble height) — never a tiny
   content-hugged pill. Keep the hit-test on the **tile root** (the bubble has `CLICKABLE`
   cleared) so the whole tile is tappable; the chrome cap is suppressed for buttons.

5. **Legibility at 160 px — font set is fixed.** Only Montserrat **14 / 20 / 28 / 38 / 48**
   and `font_xl_64` are enabled (16/18 are deliberately disabled — internal-heap glyph-cache
   starvation). `font_xl_64` is **digits + a fixed symbol set only**
   (`0-9 . - ° S T P N E W k m n d e g V A %`) — reserve it for numeric heroes; **never**
   route letters/labels (e.g. "HOME", "ROUTE") through it.

6. **Semantic colors, from `theme.*` only** (never inline a hex literal in a painter).
   `accent`/cyan = hero metric & instrument readouts; `good`/green = true wind, engaged, ok,
   positive VMG; `warn`/amber = apparent wind, the wind itself (AWS/TWD bug), caution;
   `alarm`/red = CTS, port side, alarm, negative VMG; `fg_dim` = captions/units. Apparent
   wind is **always amber**, true wind **always green** — consistent between the small
   windrose stand-in and the full dial. Honor an authored `style.color` via `value_color(m)`.

7. **Both update paths stay in sync.** Tile logic is duplicated in `update_quad_grid` (2×2)
   and the freeform update (3×3 / MIDL screens). Every tile-element change lands in **both**
   or screens diverge.

---

## 2. Compass markers

- **Glyph vocabulary is fixed:** HDG = filled **cyan** triangle; COG = hollow **green**
  triangle; CTS = filled **red** diamond; AP target = filled **amber** diamond; TWD bug =
  filled **amber** diamond. Never reuse red for anything but CTS/alarm/port.
- **Reference frame:** north-up tiles update markers with `reference=0.0` (markers at true
  bearing). Heading-up dials (AP HUD, wind-steer) use `reference=heading` so HDG rides under
  the top lubber, and rotate the scale by `-heading`.
- **Overlap is expected** (HDG≈COG when tracking course). Stagger overlapping markers
  radially (HDG/CTS inner ring, COG outer ring), draw the hollow/outline glyph last, and
  provide a **legend** (▲HDG/△COG/◆CTS) so a stacked glyph is never ambiguous.
- **NaN hides a marker** — rely on it; never draw a "0" placeholder for an absent CTS/target.
- Semicircle dials set `occlude_lower=true`; round tiles `false`.

---

## 3. Full-screen composite elements (winddial / aphud / windsteer)

A catalog element rendered **full-screen** (a single-leaf MIDL screen → ~480×480 rect) shows
a rich glass-cockpit instrument; the same element in a small tile shows a compact stand-in.
Detect with `<kind>_is_fullscreen(w,h)` (`w>=400 && h>=300`) and suppress the tile chrome
(panel + cap) for the full render.

- **Read `sk::Data` directly** in composite painters (like the legacy screens) — do **not**
  thread every sub-value through `MetricBinding`. The MIDL `value`/`dir` bindings are
  nominal for composites.
- **State lives in a PSRAM struct** (`heap_caps_calloc(MALLOC_CAP_SPIRAM)`) holding all
  handles + dirty caches — never file-statics (multi-instance) and never a large struct on a
  task stack (CLAUDE.md stack-overflow trap). Reach it via `lv_obj_set_user_data(root, st)`
  surfaced as `t.aux`, with the `t.last_aux_pct == -2` sentinel routing the update.
- **Re-derive geometry from the tile `(w,h)`**, not `LCD_W/LCD_H`. On a short leaf (a
  `[4,1]` col-split with a button row gives ~384 px), lay out explicit non-overlapping bands
  and **drop the bottom numeric tile band when `h < 440`** so the dial gets the height.
- Build on the UI task; pure LVGL objects (no PSRAM canvas → no leak).
- **Element identity, not duplication:** `wind` = situational 360° rose (AWS hero, hull/tide/
  waypoint art); `wind_steer` = forward steering aid (semicircular, laylines, HDG hero);
  `autopilot` = heading-up HUD with engage + helm feedback. They must look and function
  distinct.

---

## 4. Autopilot HUD layout & controls

- **Top bar:** `[ ENGAGE chip (L) ][ MODE badge (C) ][ HOME (R) ]`. **Center:** heading-up
  compass + HDG hero + amber target diamond (+ "TGT ddd°" caption when engaged). **Below:**
  COG/SOG line, then the **RUDDER** helm bar (the live element), then numeric tiles on tall
  leaves.
- **Engage is firmware-wired, not a MIDL button leaf** (a composite can't carry per-control
  actions): build the chip in the composite painter and `lv_obj_add_event_cb` it.
- **Engage = direct `steering/autopilot/state` PUT** via `app::post_net`, **not**
  `autopilot mode <m>` through the command funnel — `autopilot::set_mode` is permission-gated
  (`allow_engage` defaults **off**), so the command path is silently `Forbidden` on a stock
  device. Mirror the legacy `screen_autopilot.cpp::put_state`. The label flips AUTO↔STBY and
  recolors (filled green engaged / outline standby) by `apState`.
- **Mode badge** is a filled-green pill when engaged, outline when standby.

---

## 5. Center-zero indicators (rudder / XTE)

- One shared primitive: `build_centerzero_strip(parent, x,y,w,h, left_label, right_label,
  full_scale, decimals, needle_color)` (`build_xte_strip` is a thin wrapper). Instantiate per
  metric (RUDDER ±35°, XTE ±1 nm).
- **Zero is centered and labeled bare `0`**; deflection maps linearly to `±half_px`; clamp at
  full-scale. Bipolar **gauges** likewise anchor the fill at zero (0 at top-center, +cw/-ccw),
  not from the arc's left end.
- **Color by meaning, not by side:** rudder needle = cyan (instrument); XTE needle = red
  (deviation cue). Side is shown by needle position + a `P`/`S` suffix, never by coloring the
  whole bar red.
- **Sign convention:** `d.rudder` + = starboard helm; `d.xte` + = right of track. Keep the
  `P`/`S` suffixes consistent with `format_metric` / `format_xte`.

---

## 6. Wind-steering / laylines

- **Steering views are heading-up and forward-focused** (semicircle), spending pixels on the
  arc the helm steers within — reserve the full 360° rose for the situational `wind` screen.
- **Laylines are the hero, not decoration.** A red **no-go** sector centered on the wind
  (upwind: beat angle; downwind: gybe) and two green **target** sectors at the laylines should
  dominate the upper field. If they read as tiny rim chips, the substrate is wrong (use the
  semicircle).
- **Polar-aware tolerance:** the target-band width tracks TWS — wide in light air
  (`16 - 0.6·tws`, clamped 4..16°), tight in a breeze.
- **Be honest about data provenance:** when `beatAngle`/`gybeAngle` are NaN, fall back to
  45°/150° defaults but render the bands **dimmer** so an estimated layline never looks
  polar-backed. Hide sectors entirely when heading/TWD are absent.
- **TWA is the steering number; HDG is the held number** — TWA/TWD in the sub-line, HDG as
  the center hero, AWS/TWS as secondary tiles.

---

## 7. Known data dependencies (not render bugs)

Several cues only appear with the right live data, and read as "missing" on a sim that sends
partial data:
- Windrose AWA/TWA direction chevrons and the wind dial's AWA/TWS/TWA band need **wind angle**
  (`environment.wind.angleApparent/angleTrueWater`), not just speed.
- Wind-steer **laylines** need wind angle + heading (+ ideally `performance.beatAngle`/
  `gybeAngle` for real polars).
- The AP HUD **target diamond / "TGT"** needs `steering.autopilot.target.headingTrue`.
- Transient `"--"` on heading-driven dials = a momentary heading dropout; they read live when
  heading is present.
