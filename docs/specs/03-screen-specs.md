# Screen Specifications

Each screen should be optimized for a single underway decision. Secondary values
are allowed only when they help that decision.

## Dashboard

Purpose: quick scan of the boat state.

Best reference pattern:

- KIP configurable dashboard widgets.
- OpenCPN engine/system dashboard rows and bars.

Primary visualization:

- Four to six large tiles.
- Each tile has a short title, primary value, unit, and small trend/state.

Recommended tiles:

- Wind: AWS/AWA with small arrow.
- Nav: SOG/COG or heading.
- Depth: current depth with shallow color.
- Route: DTW/BTW/XTE when active.
- System: battery/tank compact bars.
- Signal K/WiFi status as a small footer, not a major tile unless degraded.

Critical states:

- Shallow depth tile turns red.
- Signal K stalled shows amber/red status.
- Missing GPS shows position/nav tile as unavailable.

## Wind

Purpose: trim and tactical wind awareness.

Best reference pattern:

- B&G SailSteer wind angle scale and target wind angle coloring.
- Raymarine Axiom wind-shift bar for headers/lifts.
- KIP compass/wind widgets.

Primary visualization:

- Circular wind dial centered on the screen.
- Bow-up apparent/true wind angle markers.
- Optional rotating compass bezel using heading.
- Close-hauled sectors for port/starboard.
- Current/tide arrow when data is available.

Required data:

- AWA/AWS.
- TWA/TWS.
- Heading.

Recommended data:

- COG.
- Current set/drift.
- Wind shift relative to mean TWD.
- Target wind angle / polar target when available.

Visual requirements:

- Apparent and true wind markers must be visually distinct.
- Port/starboard sectors should use red/green.
- If TWA is missing, hide the true marker instead of showing stale data.
- If heading is missing, keep bow-up scale and hide compass rotation.

Future enhancement:

- Add compact horizontal wind-shift bar: left = header, right = lift, center =
  mean wind direction.

## Navigation

Purpose: answer “where am I going and how fast?”

Best reference pattern:

- Classic instrument repeater: huge SOG, heading/COG, position below.
- KIP big-number widgets.

Primary visualization:

- Huge SOG in knots.
- Secondary heading and COG.
- Position in selected format: DDM default, DD/DMS optional.

Recommended additions:

- GPS fix/data age indicator.
- Small course vector arrow.
- Speed-through-water when available.

Visual requirements:

- SOG must dominate.
- Position must use a stable two-line layout.
- Missing GPS must be obvious and not confused with zero speed.

## Depth

Purpose: avoid grounding and monitor bottom trend.

Best reference pattern:

- Depth sounder large number.
- TimePlot-style depth history.
- Freeboard-SK and Signal K notifications for depth alarms.

Primary visualization:

- Huge current depth.
- 1-5 minute scrolling depth trend.
- Shallow threshold line.
- Min/max depth.

Required data:

- `environment.depth.belowTransducer` or equivalent depth path.

Recommended data:

- Water temperature.
- Configurable keel offset / below keel display.
- Alarm threshold and hysteresis.

Visual requirements:

- Current depth turns red below threshold.
- Chart should scale conservatively and avoid jumping every sample.
- Shallow threshold must remain visible even when chart auto-scales.

## Steering

Purpose: keep the boat on the desired heading/course.

Best reference pattern:

- B&G SailSteer composite view: heading, COG, wind scale, laylines, tide, rudder.

Primary visualization:

- Compass/heading arc with lubber line.
- Heading bug.
- Course-to-steer or bearing target.
- XTE bar left/right of center.

Required data:

- Heading.
- CTS or BTW.
- XTE.

Recommended data:

- COG.
- Current vector.
- Rudder angle if available.
- Target wind angle/layline when sailing.

Visual requirements:

- XTE bar should be centered, signed, and color-coded.
- CTS delta must be readable as “turn port/starboard”.
- Missing route data should degrade to heading/COG steering mode.

## Route

Purpose: show progress to the active waypoint or route.

Best reference pattern:

- Freeboard-SK route/waypoint resource model.
- B&G layline/sailing-time calculations.

Primary visualization:

- Waypoint/route header.
- DTW and BTW as primary values.
- XTE bar.
- TTG/ETA/VMG as secondary values.

Required data:

- DTW.
- BTW.
- XTE.

Recommended data:

- VMG.
- TTG.
- ETA.
- Route name / waypoint name.
- Sail-distance/time via laylines when available.

Visual requirements:

- XTE must show which side of track the vessel is on.
- If no route is active, show a clean “no active route” state.
- Do not show stale ETA without data age or speed basis.

## Autopilot

Purpose: safely view and adjust autopilot state.

Best reference pattern:

- Freeboard-SK autopilot console: engage/disengage, mode, target heading,
  heading adjustment, dodge/route operations.

Primary visualization:

- Current autopilot state.
- Target heading.
- Current heading.
- Delta from current heading to target.
- Large action buttons.

Controls:

- Standby.
- Engage/Auto.
- Adjust target: -10, -1, +1, +10 degrees.

Safety requirements:

- Buttons must be large and spaced.
- State-changing actions should show queued/pending/result feedback.
- Failed Signal K PUT should show visible warning and log.
- Autopilot commands must never block LVGL/touch.

Future controls:

- Wind mode.
- Route mode.
- Dodge port/starboard.
- Tack/gybe confirmation flow.

## Trip

Purpose: short-term passage progress.

Best reference pattern:

- Trip computer pages on commercial MFDs.
- KIP big-number and timer widgets.

Primary visualization:

- Trip distance.
- Elapsed underway time.
- Average speed.
- Max speed.

Recommended data:

- Distance over ground from GPS.
- Optional distance through water.
- Moving/stopped threshold.

Visual requirements:

- Reset must require deliberate action.
- Values must persist in NVS.
- If GPS is missing, timers may continue but distance should not accumulate.

## Status

Purpose: diagnose device, network, and vessel systems.

Best reference pattern:

- OpenCPN engine dashboard rows/bars for battery, tanks, and fluid levels.
- KIP data-state notifications.

Primary visualization:

- Two-column status rows.
- Horizontal bars for battery SOC, fuel, water, PSRAM/heap if useful.

Required data:

- WiFi state, SSID, IP, RSSI.
- Signal K state.
- Heap and PSRAM.
- Firmware build.

Recommended vessel data:

- Battery voltage/SOC.
- Fuel tank.
- Fresh water tank.
- BLE/device ID.

Visual requirements:

- Degraded network state should be colored.
- Avoid raw technical fields unless useful for debugging.
- Keep values aligned for quick scanning.

## WiFi Setup

Purpose: recover or provision connectivity without a laptop.

Best reference pattern:

- Touch-first setup wizard.
- Captive portal configuration page.

Primary visualization:

- Current AP SSID and QR code.
- Scan result list.
- Password field with on-screen keyboard.
- Saved network list.

Controls:

- Scan.
- Save/connect.
- Forget selected network.
- Forget all.

Visual requirements:

- Open networks must be supported.
- Long SSIDs must truncate safely.
- Connection/reboot pending state must be explicit.
- Captive portal mode should show simple “connect to this WiFi” guidance.

## Settings

Purpose: local device preferences.

Best reference pattern:

- KIP device/form-factor configuration.
- Commercial MFD quick settings.

Controls:

- Brightness slider.
- Day/night theme.
- Position format.
- Demo on/off.
- Trip reset.
- Open WiFi setup.

Visual requirements:

- Controls must be large enough for wet fingers.
- Settings should apply immediately where safe.
- Persistent settings must write to NVS.

## Global Overlays

### Breadcrumb

Purpose: orientation within the screen stack.

Requirements:

- Current screen title and index.
- Pips for visible screens.
- Must not consume touch except its own interactive elements.

### Alarms

Purpose: display critical state from any screen.

Requirements:

- Bottom or top banner.
- Severity color.
- Short text.
- Future: acknowledge/silence flow.

### MOB

Purpose: mark and return to man-overboard position.

Requirements:

- Long-press trigger.
- Full-screen emergency overlay.
- Bearing and distance back to MOB.
- Elapsed time.
- Long-press clear.

