# User Interface Interactions

Touch input model, gesture vocabulary, and interaction guarantees for the
ESP32 boat MFD.

## Design Goals

The device runs outdoors on a moving boat. Every gesture decision flows
from four constraints:

1. **Wet-finger / glove tolerant** — taps must accept sloppy contact;
   small targets fail.
2. **Single-handed at sea** — no two-finger gestures, no pinch.
3. **One movement, one action** — gestures must not chain. Each motion
   produces exactly one outcome.
4. **Safety-positive intent** — irreversible actions (MOB mark, MOB
   clear, autopilot engage) require a deliberate long-press, not a tap.

## Gesture Vocabulary

| Gesture | Recognition | Action |
|---|---|---|
| **Tap** | single touch, <500 ms, <12 px movement | Activate widget under finger (button, slider thumb, list row) |
| **Long-press** | single touch held ≥500 ms in place | Safety action on widgets that opt in (MOB mark, MOB clear) |
| **Swipe LEFT** | horizontal stroke ≥50 px in <500 ms, x-component dominant | Next screen in the carousel |
| **Swipe RIGHT** | mirror of LEFT | Previous screen in the carousel |
| **Swipe UP** | vertical stroke ≥50 px upward in <500 ms, y-component dominant | Open Settings overlay |
| **Swipe DOWN** | mirror of UP, downward | Return to Dashboard (home) |

### Explicitly *not* supported

- Two-finger anything (pinch, rotate, two-finger swipe) — single-handed
  operation, glove-friendly.
- Pinch-zoom — fonts/widgets are tuned for the panel; no zoom.
- Triple-tap — removed (was for legacy quadrant focus; replaced by
  proper screen carousel).
- Edge swipe (iOS-style from screen border) — collides with the
  breadcrumb chip at top and bottom edges.
- Multi-tap chords on the breadcrumb — chip is non-clickable on purpose.

## Recognition Thresholds

| Parameter | Value | LVGL key |
|---|---|---|
| Long-press hold time | 500 ms | `lv_indev_set_long_press_time(indev, 500)` |
| Swipe distance (px) | 50 | `LV_INDEV_DEF_GESTURE_LIMIT` |
| Swipe min velocity | 3 px/refresh | `LV_INDEV_DEF_GESTURE_MIN_VELOCITY` |
| Scroll vs swipe threshold | 10 px | `LV_INDEV_DEF_SCROLL_LIMIT` |
| Tap min target | 44 × 44 px | enforced per widget |

Calibration notes:

- Increasing the long-press time from LVGL's 400 ms default to **500 ms**
  reduces accidental MOB triggers from glove brushes.
- Keeping the gesture limit at **50 px** (LVGL default) on a 480-px-wide
  panel is ~10 % of width — a finger drag clearly larger than a click.

## Screen Carousel

The carousel order at boot:

```
Dashboard → Wind → Nav → Depth → Steering → Route →
            Autopilot → Trip → System
```

**Hidden** (reachable by id, not in the swipe loop):

- `wifi`  — WiFi Setup (auto-opened in AP mode)
- `settings` — Settings overlay

Carousel rules:

- Swipe LEFT advances to the next non-hidden screen, wrapping at the end.
- Swipe RIGHT moves to the previous non-hidden screen, wrapping at the
  start.
- Swipe UP from any screen opens the `settings` screen (above the
  carousel; not a carousel position).
- Swipe DOWN from any screen returns to `dashboard` (index 0).

### Breadcrumb feedback

The top-center chip shows `"<Title>  N/total"` and a row of pips beneath
it. The chip and pips are **non-clickable** so they don't eat taps
intended for screen content underneath.

## Per-Screen Interactions

### Dashboard, Wind, Nav, Depth, Steering, Route, Trip, System

Read-only screens. Taps inside the dial / numbers do nothing — these
screens exist to show, not control.

- Long-press anywhere → no effect (reserved for future "quick action"
  context menu, see *Future* below).

### Autopilot

- Tap `-10`, `-1`, `+1`, `+10` → queue `SignalKPut` of new target heading.
- Tap `ENGAGE` / `STANDBY` → queue `SignalKPut` of new autopilot state.
- All taps; no long-press required. The PUT is queued through the net
  worker so the UI doesn't block on slow HTTP.

### MOB button (global overlay)

- Top-right pill, always visible.
- **Long-press 500 ms** → captures current GPS position, opens
  fullscreen MOB rescue overlay.
- Rescue overlay: shows distance + bearing back to the mark + elapsed
  time. **Long-press 500 ms** on the "HOLD TO CLEAR" footer → exit.

### WiFi Setup

- Three sub-views: Provision (with QR), Scan list, Password entry.
- Tap a network row → moves to Password entry.
- Tap on the password textarea → keyboard shows.
- Tap `connect` → queues `SaveWifi` (device reboots after save).
- Tap `back` → return to scan list.

### Settings

Opened by swipe-UP, closed by swipe-DOWN.

- Brightness slider — drag thumb to change immediately (LEDC PWM, no
  reboot).
- Theme `day` / `night` — tap, applies via `app::post(SetTheme)`.
- Position format DDM / DD / DMS — tap to switch.
- Demo `on` / `off` — tap to toggle screen auto-cycle.
- Trip reset — tap (no long-press confirm needed since trip is local).
- WiFi setup — tap to jump to that screen.

## Alarm Banner

Bottom-center banner appears for the highest-priority active condition:

| Condition | Color | Auto-clears when |
|---|---|---|
| `SHALLOW WATER` | red | depth ≥ 3 m again |
| `SIGNALK STALLED` | amber | last delta < 10 s ago |
| `BATTERY LOW` | red | voltage ≥ 11.5 V |

Banner is non-interactive (no ack/silence yet — future).

## Visual Feedback

Touches must give a visible response even when the user can't yet act on
the result:

- Buttons render with a press-state colour on tap-down (LVGL default).
- Slider thumbs move under the finger immediately.
- Screen swap is instant (no transition animation — frame budget on the
  RGB panel doesn't reliably support cross-fades at 60 Hz).
- Breadcrumb pip updates on screen change.
- BLE / UDP log a line per recognized gesture
  (`[ui] swipe left screen=…`), useful for diagnostics.

## Hidden Surfaces and Recovery Paths

The user must always be able to reach a configuration surface:

| Symptom | Recovery |
|---|---|
| Can't see anything | Power-cycle. The device boots to Dashboard with the current saved layout. |
| Dashboard frozen | Swipe down (force-home). If that fails, send `screen dashboard` via BLE or `/api/cmd`. |
| WiFi inaccessible | Device falls into AP mode (`espdisp-setup`, open) and auto-opens the WiFi Setup screen with a QR code. |
| Wrong screen on boot | Hold the MOB clear gesture (long press 500 ms on the dark center) — future quick-reset. |
| Wedged | USB-C re-flash; the device's web UI also exposes `POST /api/cmd "reboot"`. |

## Future Extensions (Not Yet Implemented)

- **Quick-action menu** on long-press of a screen's background:
  - Wind screen → toggle TWA/AWA visibility, lock wind angle scale.
  - Steering → flip XTE polarity for windward / leeward.
  - Status → run `bench`, clear alarms, reset PSRAM stats.
- **Acknowledge alarm** by tapping the alarm banner.
- **Two-stage MOB clear** to reduce accidental clear: long-press +
  drag-to-confirm.
- **Settings sub-page** for power management (auto-dim at night, sleep
  on idle).

## Reference Hardware Behavior

- GT911 capacitive touch on this Sunton 4848S040 panel returns
  coordinates **big-endian within each 16-bit field**. Polling at 60 Hz
  on a dedicated FreeRTOS task on core 0; LVGL's `indev_read_cb` does
  one mutex copy and never touches I²C.
- Touch latency: poll-period 16 ms + LVGL frame (16 ms) = ~32 ms
  visible round-trip from contact to widget reaction.

## How Each Gesture Is Implemented (today)

| Gesture | Code path |
|---|---|
| Touch sample | `touch_task` (core 0) → `g_touch` snapshot → `touch_read_cb` (LVGL indev) |
| Tap / long-press | LVGL's `lv_indev_proc` → `LV_EVENT_CLICKED` / `LV_EVENT_LONG_PRESSED` on the touched widget |
| Swipe | `lv_indev_proc` → `LV_EVENT_GESTURE` on the active screen → `screen_gesture_handler` in `src/main.cpp` |
| Screen swap | `screen_gesture_handler` posts via `app::post(ShowScreen, "<id>")`; pump runs on the UI task |
| Autopilot PUT | Button handler posts `app::post_net(SignalKPut, …)`; net worker runs the HTTP PUT |

## Acceptance

- Every gesture in the table above produces exactly one action.
- No motion is silently ignored unless it fails the distance/velocity
  threshold.
- BLE / UDP log shows `[ui] tap …`, `[ui] swipe …`, `[touch] DOWN/UP`
  for every recognized input.
- Tap targets ≥ 44 × 44 px on every screen (verified by inspection of
  per-screen `lv_obj_set_size` calls).
- Long-press time = 500 ms exactly (verified in test or by stopwatch).
