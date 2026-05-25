# esp32-boat-mfd — work in progress

Snapshot of in-flight design / engineering threads so they can be picked up
without re-deriving context. Not a roadmap; not advice to future readers
beyond the project owner.

## Open issues observed on hardware

### 1. LVGL render loop stalls (FPS=0)
After the wind-screen redesign (commit `9a4a7ca`), repeated `bench` over
BLE returns `fps=0.0 Hz` consistently. BLE command processing still works
(different task) and `sk::loop` is running (we see `[sk] WS disconnected`
in the log), so the **main loop is alive** but **lv_timer_handler /
flush_cb appears to not be advancing**, or zero dirty areas per second.

Suspects:
- The wind dial added many LVGL primitives (rotating bezel with 16+
  children, two `lv_arc` close-hauled bands, an `lv_line` boat hull,
  wind-angle scale labels, tide arrow group). Even when hidden via
  `LV_OBJ_FLAG_HIDDEN`, all 9 fullscreen screens live in the same
  `lv_screen_active` tree and consume LVGL's invalidation / layout
  budget.
- `LV_USE_SNAPSHOT 1` was enabled in `include/lv_conf.h` (commit
  `18b555a`). Snapshot pulls in canvas-style draw paths; could affect
  rendering paths even when not invoked.

Next step (deferred): move to LVGL **native multi-screen mode** so that
only the active screen lives in the render tree:

```
// each screen creates its root with lv_obj_create(NULL) (detached screen)
// screen manager swaps via lv_screen_load(root)
// global overlays (MOB, alarm banner, breadcrumb) attach to lv_layer_top()
// so they survive screen switches without re-parenting
```

This satisfies the user's directive "inactive screens should not render"
and should restore FPS. Touch (incl. scan button) will start responding
again once LVGL is drawing.

### 2. Web UI unreachable
Curl from a peer Mac on the same `/24` returns `No route to host`.
Likely **AP isolation** on the hotspot / hotel WiFi (`MeGeNaGo` earlier
tonight, `sohohotels free+` now). Device-side fix not possible; needs a
non-isolating AP. CLAUDE.md already documents iOS Personal Hotspot has
the same gotcha.

Sanity-check workflow when web UI doesn't load:
1. `python3 tools/ble_console.py "ip"` — confirms WiFi state.
2. From a peer host: `ping <device-ip>` — if it fails, AP isolation.
3. Tether through a regular router/AP for testing.

### 3. WiFi scan button looked dead
`WiFi.scanNetworks(false, ...)` is synchronous and blocks LVGL for
2–5 s. Fixed in commit pending flash: switched to
`WiFi.scanNetworks(true, ...)` + poll `WiFi.scanComplete()` from
`refresh()`. Result list populates after the async scan finishes; status
chip shows `scanning…`.

## Pending work threads

### A. Inactive-screen rendering (high prio — fixes FPS stall)
Refactor `src/ui/screen_*.cpp` so each `build()` returns a detached
screen object (`lv_obj_create(NULL)`). Update `screen_manager.cpp` to
swap via `lv_screen_load_anim(root, LV_SCR_LOAD_ANIM_NONE, 0, 0, false)`
instead of toggling `LV_OBJ_FLAG_HIDDEN`. Move MOB button / alarm
banner / breadcrumb onto `lv_layer_top()`.

### B. WiFi setup from web UI
Two new endpoints + UI section on `/`:

- `POST /api/wifi/scan` → triggers async scan, returns immediately.
- `GET  /api/wifi/networks` → returns last completed scan result
  (id, rssi, secured).
- `POST /api/wifi/connect` body `{"ssid":"...", "password":"..."}` —
  also accept empty/omitted password for open networks. Routes through
  `net::dispatchCommand("wifi <ssid> <pass>")`.

Mirror in BLE Connection characteristic if simple.

### C. Captive portal walkthrough — open hotel networks
Use case the owner has tonight: `sohohotels free+`, open AP, requires a
captive-portal form submit.

Owner's portal credentials (kept here so the device doesn't need to
prompt or store them at runtime — re-typed in code only when the actual
portal HTML is known):

```
Name:    Boris
Surname: Sorochkin
Room:    210
Email:   sbornava@gmail.com
```

Proposed device flow:
1. After STA join, probe `http://www.google.com/generate_204`.
2. If response code != 204 (typical captive portal redirect), capture
   the redirect `Location:` header — that's the portal's landing page.
3. GET the landing page, extract `<form>` `action=` URL and field names.
   (Hotels often use generic field names: `firstname`, `lastname`,
   `room`, `email`, `accept` checkbox.)
4. POST the form with the credentials above.
5. Re-probe `generate_204`. If 204, internet works.

Heuristics required because every chain (Mikrotik, UniFi, Aruba ClearPass,
hotel-specific) has different field names. Acceptable first-pass: hardcode
the field-name set we observe at this hotel; fall through to a "manual
portal" hint surfaced on the WiFi setup screen ("open this URL on your
phone to finish login").

### D. OpenHASP-style runtime addressing (#20)
Renderer becomes data-driven. Layout JSON describes widgets per screen;
runtime addressing `screenId.widgetId.attr` exposed via:
- `PUT /api/widget/<screen>/<id>/<attr>` over HTTP
- BLE Configuration characteristic
- Console: `set wind.aws.text "12.4"`

Required to make `PUT /api/layout` actually change the UI (today it's
round-tripped but ignored — screens are hardcoded C++). Unblocks #33
(design session via web UI).

### E. JSONL layout format (#22)
Accept newline-separated single-widget records as an alternative to the
big nested JSON. Useful for incremental updates. Detect by leading `{` on
a newline boundary.

### F. Screenshot endpoint (currently 503)
`/api/screenshot.bmp` is disabled because `lv_snapshot_take` walks the
LVGL tree and isn't safe from the web task. Re-enable by marshalling the
request through a queue back to the LVGL task; the LVGL task takes the
snapshot, posts the buffer back. Add a semaphore so the web task can
block on completion and stream the BMP.

## Commits already on `main`

```
9a4a7ca feat(wind): redesign dial inspired by marine-nav-compass-card
4b317c6 fix(web): move HTTP server to dedicated core-0 task
18b555a feat: on-device web UI + design API (screenshot, state, layout)
d17086a feat: hidden-screen flag, breadcrumb, auto-open WiFi setup
6a79f31 feat: add Route, Autopilot, and on-screen WiFi setup screens
3827954 feat: screen manager + redesigned fullscreen panels
```

## Tasks (TaskList state)

Pending: #11 (marine charts — needs SK chart plugin), #20 (OpenHASP
addressing), #22 (JSONL), #26 (companion phone app — separate project),
#33 (re-run design session, blocked on #20/#21).

Completed in this push: the full screen-manager refactor, all
fullscreen redesigns, swipe nav, day/night theme, MOB / alarms,
trip / route / autopilot / wifi-setup screens, web UI v1, breadcrumb,
auto-open-wifi-setup-on-AP.
