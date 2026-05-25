# Visual Reference Model

This project targets a compact 480x480 sailing MFD. The UI should borrow proven
marine visualization patterns without copying proprietary artwork. The goal is
fast cockpit readability, touch tolerance, and predictable layouts.

## Reference Systems

### Signal K KIP

KIP is a Signal K MFD and instrument panel focused on touch-first dashboards,
custom widgets, gauges, charts, switches, night/day mode, fullscreen/kiosk
operation, and role/form-factor-specific configurations.

Use for:

- Configurable dashboards.
- Widget-based layout thinking.
- Touch-first dashboard navigation.
- Day/night modes.
- Data-state notifications on widgets.

Reference: https://github.com/mxtommy/Kip

### Freeboard-SK

Freeboard-SK is a Signal K chartplotter. Its useful patterns are moving maps,
routes/waypoints, alarms, AIS, weather layers, true/apparent wind display,
autopilot console, resource-backed charts, and instrument drawers.

Use for:

- Future chart screen.
- Route/resource model.
- Alarm and notification model.
- Autopilot command concepts.
- Chart plus instrument overlay separation.

Reference: https://github.com/SignalK/freeboard-sk

### B&G SailSteer / Laylines

B&G sailing displays popularized the composite sail-steering view: heading,
COG, wind angle scale, tide/current vector, current waypoint, true/apparent
wind, port/starboard laylines, rudder angle, and target wind angle coloring.
B&G laylines can use actual wind angle, manual wind angle, or polar target wind
angle, and can include tidal flow correction and historic layline limits.

Use for:

- Steering screen.
- Wind screen.
- Route screen.
- Future performance / polar features.

References:

- https://manualzz.com/doc/54412125/bandg-zeus-touch-instruction-manuals
- https://manualzz.com/doc/31509004/bandg-vulcan-chartplotter-operator-manual

### Raymarine Axiom Wind Shift

Raymarine’s wind-shift feature uses numerical and horizontal bar displays to
show headers/lifts around an averaged wind direction. This is a compact,
readable visualization that fits small screens better than complex trend plots.

Use for:

- Wind screen.
- Steering screen.
- Future race/performance mode.

Reference: https://www.raymarine.com/en-us/learning/online-guides/axiom-wind-shift-display

### OpenCPN Engine Dashboard

OpenCPN’s engine dashboard emphasizes simple rows, bars, fluid levels, battery
state, and engine/system parameters from NMEA/Signal K sources.

Use for:

- System screen.
- Electrical/tank widgets.
- Status rows and horizontal bars.

Reference: https://opencpn.org/OpenCPN/plugins/enginedashboard.html

## Global Visual Rules

- The active screen must be readable at arm’s length in sunlight.
- Primary value text should be the largest visual element unless the screen is
  primarily spatial, such as Wind, Steering, Route, or future Chart.
- Use color only for meaning:
  - Red: alarm, port, danger, shallow water.
  - Green: good, starboard, target/on-course.
  - Amber/yellow: caution, waypoint/target, pending.
  - Blue/cyan: water/current/network/secondary vectors.
- Every screen must have an obvious missing-data state.
- Avoid dense text labels. Prefer short labels, symbols, bars, needles, vectors,
  and stable positions.
- Do not rely on animation for critical information.
- Preserve day/night theme support.

