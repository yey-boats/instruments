# Layout Presets

These presets define recommended screen stacks for common users. They should be
expressible in the JSON layout system and selectable from web/BLE/Signal K.

## Cruiser Default

For general cruising and coastal navigation.

Order:

1. Dashboard
2. Wind
3. Nav
4. Depth
5. Route
6. Steering
7. Trip
8. Status

Hidden:

- Autopilot
- WiFi Setup
- Settings

Rationale:

- Prioritizes depth, speed, wind, and route progress.
- Keeps operational screens in the swipe loop.
- Hides configuration/control screens from accidental swipes.

## Sailing Performance

For active sailing and trimming.

Order:

1. Wind
2. Steering
3. Route
4. Dashboard
5. Nav
6. Depth
7. Trip

Hidden:

- Autopilot
- Status
- WiFi Setup
- Settings

Rationale:

- Matches B&G SailSteer-style workflows.
- Makes wind/steering/route the primary loop.
- Keeps depth close but not dominant.

## Shallow Water

For canals, rivers, anchorages, and poorly charted areas.

Order:

1. Depth
2. Dashboard
3. Nav
4. Route
5. Status

Hidden:

- Wind
- Steering
- Autopilot
- Trip
- WiFi Setup
- Settings

Rationale:

- Makes depth trend and shallow alarm the first screen.
- Keeps navigation and route close.
- Avoids clutter from sailing-specific screens.

## Autopilot Passage

For motoring or sailing with active autopilot.

Order:

1. Autopilot
2. Route
3. Steering
4. Dashboard
5. Nav
6. Wind
7. Depth
8. Status

Hidden:

- WiFi Setup
- Settings

Rationale:

- Puts autopilot state and target first.
- Keeps route and cross-track error next.
- Keeps manual situational awareness one swipe away.

## Diagnostics

For development, setup, and troubleshooting.

Order:

1. Status
2. WiFi Setup
3. Settings
4. Dashboard
5. Nav
6. Wind
7. Depth

Hidden:

- Autopilot
- Route
- Trip

Rationale:

- Exposes device health, networking, and config immediately.
- Useful for boatyard/lab testing.

## Preset JSON Sketch

```json
{
  "version": 1,
  "settings": {
    "default_screen": "dashboard",
    "preset": "cruiser_default"
  },
  "screens": [
    { "id": "dashboard", "title": "Dashboard", "type": "dashboard" },
    { "id": "wind", "title": "Wind", "type": "wind" },
    { "id": "nav", "title": "Nav", "type": "nav" },
    { "id": "depth", "title": "Depth", "type": "depth" },
    { "id": "route", "title": "Route", "type": "route" },
    { "id": "steering", "title": "Steering", "type": "steering" },
    { "id": "trip", "title": "Trip", "type": "trip" },
    { "id": "status", "title": "System", "type": "status" },
    { "id": "autopilot", "title": "Autopilot", "type": "autopilot", "hidden": true },
    { "id": "wifi", "title": "WiFi Setup", "type": "wifi_setup", "hidden": true },
    { "id": "settings", "title": "Settings", "type": "settings", "hidden": true }
  ]
}
```

