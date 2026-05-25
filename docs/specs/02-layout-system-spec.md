# Layout System Specification

The layout system should describe what appears on the device without forcing
the ESP32 to become a general web dashboard engine. Keep layout documents
small, bounded, and safe for embedded parsing.

## Goals

- Allow server-managed and BLE/web-applied layouts.
- Support fixed high-quality screen types for cockpit use.
- Support widget addressing for tuning labels, colors, thresholds, and paths.
- Keep parsing bounded and host-testable.
- Keep rendering native LVGL, not HTML.

## Non-Goals

- Arbitrary CSS-like layout.
- Infinite dashboards.
- Embedded JavaScript.
- General chartplotter replacement.
- Long-term historical analytics.

## Layout Document

The existing JSON shape remains the base:

```json
{
  "version": 1,
  "settings": {
    "default_screen": "dashboard",
    "demo_period_ms": 3000,
    "theme": "night",
    "units": "metric"
  },
  "screens": [],
  "alarms": []
}
```

## Screen Object

```json
{
  "id": "wind",
  "title": "Wind",
  "type": "wind",
  "hidden": false,
  "paths": {},
  "widgets": [],
  "settings": {}
}
```

Rules:

- `id` is stable and used for navigation and widget addressing.
- `type` selects one of the native screen renderers.
- `hidden` means reachable by command/API but skipped by swipe cycle.
- `paths` binds screen-level Signal K values.
- `widgets` is optional for future per-widget customization.

## Widget Addressing

Use OpenHASP-style addressing:

```text
screen_id.widget_id.attribute
```

Examples:

```text
wind.awa_marker.color
nav.sog_value.font
depth.shallow_threshold.value
status.battery_bar.warn_lt
```

Required API:

- `GET /api/widgets`
- `PUT /api/widget/<screen>/<id>/<attr>`
- Console: `set wind.aws_value.color "#ffffff"`
- BLE: same command over Configuration characteristic or NUS.

## Built-In Screen Types

Required:

- `dashboard`
- `wind`
- `nav`
- `depth`
- `steering`
- `route`
- `autopilot`
- `trip`
- `status`
- `wifi_setup`
- `settings`

Future:

- `chart`
- `race_start`
- `performance`
- `anchor`

## Data Binding

Use Signal K paths directly. The firmware parser should continue to map common
paths into `sk::Data` for fast native screens, but layout paths should remain
explicit so screens can evolve without recompiling every mapping.

Example:

```json
{
  "type": "wind",
  "paths": {
    "awa": "environment.wind.angleApparent",
    "aws": "environment.wind.speedApparent",
    "twa": "environment.wind.angleTrueWater",
    "tws": "environment.wind.speedTrue",
    "heading": "navigation.headingTrue",
    "current_set": "environment.current.setTrue",
    "current_drift": "environment.current.drift"
  }
}
```

## Alarm Rules

Alarm rules remain bounded:

```json
{
  "id": "shallow",
  "path": "environment.depth.belowTransducer",
  "level": "alarm",
  "lt": 3.0,
  "message": "SHALLOW WATER"
}
```

Recommended additions:

- `hysteresis`
- `delay_ms`
- `clear_delay_ms`
- `screen_hint`
- `ack_required`

## Storage And Safety

- Keep the last good layout cached.
- Keep a baked-in fallback.
- Validate before applying.
- Reject layouts above a fixed maximum.
- Apply layout only on the LVGL/UI task.
- Provide snapshot-copy APIs for web/BLE reads.

