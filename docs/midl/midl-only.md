# MIDL-only screens: device ↔ Display Manager contract

Status: decided by the owner on 2026-11-15. This extends
`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md` §3.6–3.8
and supersedes the transition plan in `docs/midl/manager-device-sync.md`.

## Decisions
1. The display renders **MIDL screens only**. Display Manager (DM) stores and
   delivers **MIDL only**. The v2 widget/tile plan, DM's MIDL↔v2 adapter, the
   firmware's managed-screens pipeline, the legacy `layout::Config` pipeline and
   the hand-written C++ content screens are removed.
2. Built-in screens become **embedded MIDL records** baked into the firmware.
   One shared source: `midl/library/builtin/*.midl.json` in the midl repo,
   consumed by the firmware (embedded at build time) and by DM (its built-in
   presets) through the midl submodule.
3. Delivery: **push is the default** (DM → device HTTP); **pull** (in the config
   the device already fetches from DM) is also supported. Both write the same
   record store on the device.
4. **Setup UI stays system UI** outside MIDL: settings, Wi-Fi setup, touch
   calibration and touch grid. They are never removed by a MIDL apply and are
   never in the carousel.
5. The Waveshare knob (round-360) goes MIDL-only in the same change.

## Records
A **record** is one MIDL document plus metadata:

```
{ id, title, source: "embedded" | "manager" | "local", hash, doc }
```

- `id`: `[a-z0-9_-]{1,32}`. `hash`: `sha256:<hex>` of the exact doc bytes
  stored.
- A doc may hold several screens. **Screen ids are unique across all records
  on a device.** A write whose screens collide with another record's screens
  is rejected with 409 `screen_id_conflict` naming both records.
- A manager or local record with the **same id as an embedded record
  overrides it**. Deleting the override restores the embedded one.
- Embedded record ids and their screens match today's built-in screen ids, so
  existing `screen.set` ids keep working: `dashboard`, `wind`, `wind_classic`,
  `wind_steer`, `nav`, `depth`, `steering`, `route`, `autopilot`, `trip`,
  `status` (square/rect classes), and the knob's `ap_hud`, `knob_compass`,
  `knob_wind`, `knob_big` (round-360). One embedded record per built-in screen.
  A record carries `variants` for each class the firmware ships for.

### Index
```
{ order: [recordId...], defaultScreen: "<screenId>", hidden: [recordId...] }
```
`order` is the carousel order of records. A record missing from `order` goes
after the listed ones in embedded-then-id order. `defaultScreen` is the home
screen: boot, swipe down, Select and the HUD HOME chip go there (no more
hard-coded `dashboard`). The default is the first screen of the first visible
record.

## Device behaviour
- **One record is active at a time**; the device renders it with
  `midl::render::apply_all`. `screen.set <screenId>` shows the screen if it is in
  the active record. Otherwise the device **loads the owning record** (from
  flash, or the embedded bytes) and then shows the screen. Swiping past the
  last screen of a record loads the next visible record in `order` (first
  screen), and before the first loads the previous one (last screen).
- System screens are registered so that `ui::reset_screens()` keeps them.
- **Persistence (LittleFS)**: `/midl/index.json`, `/midl/records/<id>.json`
  (record JSON as above), `/midl/records/<id>.lkg.json` (last known good: promoted
  after a record renders cleanly). Boot: load the index and render the record
  owning `defaultScreen`. If a record fails to render, fall back to its LKG copy,
  then to the embedded record of the same id, then to the default embedded
  record. **Factory** = the embedded records (reset deletes `/midl/`).
- Every write is validated against the device's own embedded manifest
  (`midl::validate::admit`) before it is stored; 422 with a JSON pointer.
- **Text-valued local ids** for the status record: `net.ip`, `net.ssid`,
  `net.rssi`, `sk.state`, `sk.host`, `sys.build`, `sys.version`, `sys.uptime`,
  `sys.heap`, `device.id`. Unknown ids render `--`.

### Push API (device HTTP; auth = `X-YeyBoats-Authorization: Bearer <manager device token>` OR web Basic auth)
| Method | Path | Body / query | Result |
|---|---|---|---|
| GET | `/api/midl/manifest` | | existing |
| GET | `/api/midl/records` | | `{active, screen, index, records:[{id,title,source,hash,overridesEmbedded,screens:[{id,title}]}]}` |
| GET | `/api/midl/records/<id>` | | the record JSON |
| PUT | `/api/midl/records/<id>` | `{title, doc}`; `?activate=1&screen=<sid>` | 200 `{id,hash,screens}`; 422 invalid; 409 conflict; 413 too large (48 KB doc cap) |
| DELETE | `/api/midl/records/<id>` | | removes a manager/local record or override; 404 for a bare embedded record |
| PUT | `/api/midl/index` | index JSON | 200 |
| POST | `/api/midl/reset` | `?to=factory` | deletes `/midl/`, renders the embedded default |
| POST | `/api/midl/config` | a bare MIDL doc | kept: alias of PUT record `local` + activate |

A write from the manager stores `source:"manager"`; a write with web Basic auth
only stores `source:"local"`.

### Pull (manager config)
`GET /devices/<id>/config` keeps every non-screen key (settings, sources,
nmea0183Wifi, autopilot, ota, webAuth, network, debug, …) and **replaces**
`layout`/`widgets` with:

```
"midl": {
  "index": { "order": [...], "defaultScreen": "...", "hidden": [...] },
  "records": [ { "id", "title", "hash", "url": "/devices/<id>/midl/records/<rid>" } ]
}
```

The device compares each `hash` with its store and fetches only changed
records (`GET url` → record JSON, device-token auth). It deletes `manager`
records that are not listed, and leaves embedded and `local` records alone.
`config.hash` covers the `midl` block. A record that already arrived by push has
a matching hash, so a later pull is a no-op. The device ignores `layout` and
`widgets` keys.

### Heartbeat / identity
- Heartbeat `ui`: `screen`, `record`, `screens:[{id,title,record}]` (all screens
  across visible records, in carousel order), `theme`, `brightness`.
  New `midl: {version, class, indexHash, records:[{id,hash,source}]}`.
  The legacy `layoutVariant`, `widgetVariant`, `widgetConfigHash` and v2
  `capabilities` (viewTypes/maxViews/maxTilesPerScreen) go away.
- Identity `capabilities.midl = {version, class, manifestHash, push:true,
  pull:true}`; the legacy `capabilities.widgets` / `capabilities.layout` go away.

## Display Manager
- A profile (preset/view) holds **MIDL records** + an index:
  `profile.midl = {records:[{id,title,doc,source,sourceId?,sourceUpdatedAt?}], index}`
  next to the non-screen `profile.config` (settings, sources, …). No
  `layout`/`widgets`.
- **Built-in presets** = the embedded records from `midl/library/builtin`. The
  default profile lists them by reference, so a device on the default profile
  needs nothing pushed.
- **Import** (file and "From yey.boats") stores the MIDL doc unchanged as a
  record, validated against the target device class manifest (prefer the
  device's own `/api/midl/manifest` when DM has it; else the bundled class
  manifest). Spans, windrose and every other valid MIDL feature are kept.
- **One-time migration** of stored v2 profiles to records at load, using the
  adapter's v2→MIDL direction one last time (then the adapter is deleted).
- **Delivery**: on save or assign, DM **pushes** changed records + the index
  (`PUT /api/midl/records/<id>`, `PUT /api/midl/index`) with the device token.
  If the device is unreachable or refuses auth, DM queues `config.reload` and
  the device pulls. `screen.set` uses bare MIDL screen ids.
- The preview renders the stored record docs with the MIDL web renderer (no
  flattening). The v2 layout and field editors are removed; the MIDL editor
  edits records.

## Out of scope
- New MIDL elements for forms/keyboards (setup UI stays native).
- Changes to non-screen config.

## Firmware implementation notes (instruments, phase 1)

Wire details as implemented on the device. Where they refine the contract
above, the reason is given; Display Manager (Instruments-manager `feat/midl-only`)
matches these.

### Embedded records
- Source: `midl/library/builtin/<id>.midl.json` (midl submodule), embedded per
  display class by `tools/embed_builtin_records.py` (`make embed-records`) into
  `include/midl_gen/<class>/builtin_records.h`, selected by
  `include/generated_midl_builtin.h`. A record ships on a class when one of its
  screens declares a `variants` entry for that class: square/landscape classes
  get the 11 built-in screens, round-360 gets the knob views.
- Embedded hash = `sha256:` + hex of the compact doc bytes, which equal
  `JSON.stringify(doc)` (checked against Node for every built-in).
- "Embedded order" (for records missing from `index.order`) is the built-in
  table order (dashboard, wind, wind_classic, wind_steer, nav, depth, steering,
  route, autopilot, trip, status / ap_hud, knob_compass, knob_wind, knob_big),
  then other records by id.

### Hashes
- Manager writes (push or pull) store the hash Display Manager supplies
  (`hash` in the PUT body / the pulled record JSON / the config entry) and
  report it verbatim: ArduinoJson may format floats differently from JS, so
  the device never re-hashes a manager doc. Local writes are hashed by the
  device over its own compact serialization.

### Push API
- Auth: `X-YeyBoats-Authorization: Bearer <device token>` matching the token the
  manager issued at registration -> source `manager`, even when Basic auth is
  sent too. A wrong/stale bearer falls back to valid Basic credentials (source
  `local`), else 401 `{"ok":false,"error":"bad_manager_token"}`. No bearer:
  the web Basic-auth policy, source `local`.
- `PUT /api/midl/records/<id>` body `{title?, doc, hash?}`,
  `?activate=1&screen=<sid>`. 200 `{ok,id,hash,source,screens:[ids]}`.
  Errors: `{ok:false,error:<code>,message,record,path?,conflictWith?,screen?}`;
  400 `bad_id|bad_doc|bad_hash`, 413 `too_large`, 422 `invalid_doc` (+`path`),
  409 `screen_id_conflict` (`record` = the written record, `conflictWith` = the
  owner, `screen` = the colliding id), 507 `store_full` (24 stored records).
- `DELETE /api/midl/records/<id>`: 404 `embedded_record` for a bare built-in,
  404 `not_found` for an unknown id.
- `GET /api/midl/records/<id>`: the record JSON (built-ins too).
- `GET /api/midl/index` (extra) and `PUT /api/midl/index` -> `{ok,indexHash}`.
- `POST /api/midl/reset` (`?to=factory`, the default). The old `to=previous`
  is gone (records keep a last-known-good copy instead).
- `POST /api/midl/config` = `PUT /api/midl/records/local?activate=1` with a
  bare doc; `GET /api/midl/config` returns the active record's doc.

### Pull
- `cfg.midl.records[].url` is relative to the manager endpoint (absolute
  `http…` urls also work); fetched with the same headers as `/config`. The
  response `{id,title,source,hash,doc}` must name the same id.
- The MIDL-records build never applies `layout`/`widgets` (managed screens),
  with or without a `midl` block.

### Local ids
- Text: `net.ip`, `net.ssid`, `net.rssi` ("-61 dBm"), `sk.state`, `sk.host`
  ("host:port"), `sys.build`, `sys.version`, `sys.uptime` ("3d 04:05" /
  "04:05:06"), `sys.heap`, `sys.psram` ("123 KB" / "7.5 MB"), `device.id`,
  `trip.underway` ("H:MM:SS").
- Numeric (display units): `trip.distance` (nm), `trip.avgSpeed`,
  `trip.maxSpeed` (kn), plus the earlier `temperature`, `heap`, `psram`, `rssi`,
  `uptime`. The trip log integrates SOG whatever screen is shown
  (`src/trip_log.cpp`).

### Navigation
- `ShowScreen "home"` = `index.defaultScreen` (swipe down, Select, HUD HOME,
  settings Close). Swipe down on settings returns to the screen it was opened
  from. The console `screen <id|next|prev|N>` goes through the same funnel.

## Firmware phase 2: legacy pipelines removed (instruments)

The `YEYBOATS_LEGACY_SCREENS` build flag and everything it kept compiling are
gone; the device has no non-MIDL screen path left.

### Removed
- The C++ content screens (dashboard, nav, route, steering, depth, trip, wind,
  wind_classic, wind_steer, autopilot, status, demo_grid, the "zoom" screen)
  and the knob's C++ round views (ap_hud, knob_compass, knob_wind, knob_big).
  The ids live on as the embedded records.
- Managed screens: `manager_config` RenderPlan parsing, `manager_screens`,
  `widget_registry`, `font_resolver`, `ApplyManagedScreens`, the console
  `manager-layout` / `manager-widgets` / `font-dump`.
- The `layout::Config` pipeline: `layout_loader`, `layout_renderer`, boot
  `layout::load_default()`, `ApplyLayout`, `/api/layout`,
  `/api/dashboard/config.{json,yaml}`, `layout-show` / `layout-fetch` /
  `layout-load`. `layout.h` keeps only the shared bounds (`MAX_SCREENS`,
  `STR_LEN`, `PATH_LEN`) and `layout::zoom_action`.
- The RAM-only `midl_doc` store, `ConfigApplyMidl`, `midl::render::apply_doc`,
  the `midl-render` command (`include/midl_demo_doc.h` stays as a test/sim
  fixture), `view q0..q3`, `wind-refresh`.
- The v2 capability manifest (`capabilities.cpp`) and the heartbeat/identity
  fields listed under "Heartbeat / identity" above. `config.ui.default_screen`
  (the home screen is `index.defaultScreen`).
- The template painters only the C++ screens used (QuadGrid, HeroPlus,
  StatusList, RoundInstrument, SplitPair, TrendChart, AlertFocus,
  ControlConsole, RouteProgress, SetupForm, `ui::layouts::create/update`,
  `TemplateId`). The freeform painter and its per-kind painters, including the
  winddial / windsteer / aphud HUD composites, stay.
- `design/midl/*.json` (superseded by `midl/library/builtin`), the sim envs
  `sim`, `sim-wind`, `sim-ap`, `sim-knob`, `tools/render_all_resolutions.sh`.

### Changed device surfaces
- `GET /api/screens` lists the whole carousel across the visible records
  (`{index,id,title,record,hidden:false,active}`), then the hidden system
  screens. Before, it listed only the registered screens of the active
  record.
- BLE CONFIGURATION (`…e003…`) is read/notify only and returns a status
  summary `{midl:1, record, screen, home, records}`. Writes are gone (a
  512-byte attribute cannot carry a record).
- `/api/security` `write_endpoints` lists `/api/midl/records/<id>` and
  `/api/midl/index` instead of the layout endpoints.

### Knob (round-360)
The knob shows its embedded records only. The rotary menu is system UI on
`lv_layer_top()` (`src/knob_menu.cpp` state machine, host-tested):

| Level | Turn | Click | Double-click | Long-press |
|---|---|---|---|---|
| Home (MIDL screen) | next / prev screen | home screen | Steer | Menu |
| Steer (shows `ap_hud`, STEER badge) | AP target ±1° (±5° held) | engage / standby | Home | Mode picker |
| Menu: STEER / AP MODE / DISPLAYS / CLOSE | highlight | select | Home | Home |
| Mode picker | highlight | set mode, back to Steer | back to Steer | |
| Displays / Views | highlight | enter / switch view | back | |

### Rendering fixes in the same change
- Text elements fit their tile: the font steps down 48/28/20/14 px (capped at
  the painter's size) until the value fits the tile width, else the value is
  ellipsized at the end. VERSION / BUILD on the status record were clipped at
  both edges.
- Trend elements show `format.unit` next to the current value, like
  single-value tiles.
- The freeform painter now snapshots the display format config
  (`config::format()`) each refresh; only the removed template path did.
