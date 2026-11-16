# UI Appearance Redesign Session

Status: active design direction. This records the visual decisions from the
first redesign pass so remaining screens can converge on the same language.

## Goals

- Improve cockpit readability without increasing visual clutter.
- Move away from a one-note dark-blue interface.
- Keep semantic colors stable: red/port/alarm, green/starboard/good,
  amber/caution/wind, cyan/navigation.
- Make screens feel like instruments, not generic app cards.
- Preserve day/night theme support and 480x480 constraints.

## Visual Language

### Adopted Marine Instrument Concepts

From the NMEA2000 display comparison, adopt the ideas that make the UI read as
a real cockpit instrument:

- white or very light daylight instrument faces for wind, steering, route, and
  settings
- shared round bezels with tick rings, lubber markers, and 30/60/90/120/150
  angle labels
- red/port and green/starboard sectors on wind and steering instruments
- center value blocks inside round instruments
- small corner secondary values around the main instrument
- large, mode-group controls for autopilot and settings
- dense framed data panels with aligned labels and values
- route/steering spatial view with boat, track line, and XTE context

Keep the current native LVGL implementation and theme helpers. Do not adopt
SquareLine generated code, full bitmap UI, script branding, or heavy beveled
shadows as the default visual system.

### Palette

Night mode:

- green-black background
- dark desaturated green panels
- muted green-gray borders
- near-white primary text
- gray-green secondary text
- cyan navigation accent
- amber warning/wind accent

Day mode:

- warm white background
- dark green-black text
- saturated but non-neon semantic accents
- optional white instrument-face panels with gray tick marks and dark labels

### Panels

Panels use:

- 6 px radius
- 1 px semantic border
- compact 10 px padding
- a 4 px accent rail near the top-left

The accent rail is the primary visual cue for panel type. Do not add large
decorative gradients or image backgrounds.

Round instrument panels may use a visible bezel/tick-ring instead of the accent
rail when the circular geometry is the primary visual cue.

### Typography

- Captions: B612 14 (Regular), dim text.
- Standard values: B612 20 (Regular) or 28 (Bold).
- Hero values: B612 48 (Bold).
- Use color on hero values only when it communicates status or domain.

### Screen Roles

- Dashboard: four instrument panels with semantic accents.
- Nav: SOG is the hero; COG/HDG are secondary instruments.
- Depth: depth value and chart are green/good until alarm threshold.
- System: dense operational rows inside one framed instrument panel.
- Wind: still spatial/instrument-first; needs a follow-up pass after render
  profiling because the bezel/markers are performance-sensitive.
- Autopilot: large mode group and heading-step buttons, with explicit pending
  state.
- Route/Steering: spatial track/XTE context, not only numeric tiles.

The follow-up layout direction is to implement these roles as reusable
templates, then make screens variants of those templates. See
`11-layout-templates-screen-variants.md`.

## Implemented In First Pass

Files:

- `include/ui_theme.h`
- `src/ui/ui_theme.cpp`
- `src/ui/screen_dashboard.cpp`
- `src/ui/screen_nav.cpp`
- `src/ui/screen_depth.cpp`
- `src/ui/screen_status.cpp`

New helper API:

```cpp
void style_screen(lv_obj_t *o);
void style_panel(lv_obj_t *o, uint32_t accent = 0);
void style_caption(lv_obj_t *o);
void style_value(lv_obj_t *o, const lv_font_t *font, uint32_t color);
lv_obj_t *panel_accent(lv_obj_t *parent, uint32_t color);
```

Use these helpers for new or updated screens instead of repeating raw panel
styling in every screen module.

## Remaining Screens

Apply the same system to:

- Wind
- Steering
- Route
- Autopilot
- Trip
- WiFi setup
- Settings

Order:

1. Steering and Route, because they are spatial navigation screens.
2. Autopilot, because semantic action/status color matters.
3. WiFi and Settings, because they need touch feedback and clear working
   states.
4. Wind, after profiling decides whether the bezel should remain as LVGL
   rotated objects or move to a cheaper static/canvas representation.

Adopt the round-instrument visual language during the Wind/Steering/Autopilot
passes.

## Acceptance

- No text overlaps at 480x480.
- Every screen has an obvious primary value or spatial instrument.
- Missing data states remain readable.
- Day/night modes preserve contrast.
- `pio run -e esp32-4848s040` passes.
- Hardware screenshot pass confirms panels, labels, and breadcrumb do not
  collide.
