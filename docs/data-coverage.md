# Data Coverage Reference

Status: derived directly from source on `feat/2026-07-mfd-overhaul` (working
tree, not `HEAD`). This is the authoritative list of every protocol message
and data path the firmware understands, with file/function references so it
can be re-derived by grep. Where behavior looked ambiguous from source alone
it is marked **verify on hardware** rather than guessed.

## Ingest sources and arbitration

Three producers publish into one source-neutral fused model
(`boat::Snapshot`, `include/boat_data.h`):

- **NMEA2000** (`src/source_nmea2000.cpp`, `include/n2k_decode.h`) — CAN bus
  via TWAI, held **listen-only** always; gated behind the `ENABLE_NMEA2000`
  build flag (see below).
- **NMEA0183-over-WiFi** (`src/source_nmea_wifi.cpp`, `src/nmea0183_parser.cpp`,
  `include/nmea0183.h`) — UDP (default, port 10110) or TCP, configured via
  `nmea-wifi` console commands.
- **SignalK** (`src/signalk.cpp`, `src/signalk_parser.cpp`) — WebSocket
  client, the primary/default path.

Each producer calls `boat::publish(&Snapshot::<field>, SourceKind, now_ms,
value)` (`src/boat_data.cpp`). `boat::should_accept()` decides whether an
incoming publish may overwrite the field currently held:

- Same-or-higher priority than the current owner: always accepted.
- Lower priority: only accepted if the current owner's value is **stale**
  (older than that source's timeout).

Default priority (`boat::Priority`, `include/boat_data.h`): **NMEA2000 >
NMEA0183-WiFi > SignalK > Demo** (physical bus wins when present). Default
per-source timeouts (`boat::Timeouts`): NMEA2000 2000 ms, NMEA-WiFi 3000 ms,
SignalK 10000 ms, Demo 60000 ms. Both are runtime-configurable
(`boat::set_priority` / `boat::set_timeouts`).

SignalK ingest additionally carries a **per-field touched mask**
(`boat::FieldMask`, one bit per `boat::FieldId`). `sk::applyDelta`
(`src/signalk_parser.cpp`) sets a bit only for fields the current delta
actually carried; `boat::ingest_signalk` (`src/source_signalk.cpp`) republishes
only those bits. This preserves per-field staleness — a sensor that stops
reporting is not kept artificially "fresh" just because other fields in the
same WebSocket stream keep updating. NMEA2000/NMEA0183 publish directly,
field-by-field, as each message is decoded, so the same staleness logic
applies per `Field.updated_ms`/`Field.source` without a separate mask.

## 1. NMEA 2000 PGNs

Dispatch: `nmea2000::decode_pgn()` in `src/source_nmea2000.cpp` (a `switch` on
PGN, called from the TWAI RX worker). Pure payload decode lives in
`include/n2k_decode.h` (host-testable; no TWAI/Arduino deps). Fast-packet PGNs
are reassembled per `(PGN, source address)` through
`n2k::FastPacketPool`/`fastpacket_pool_feed` — a 4-slot pool, because
interleaved AIS bursts from several source addresses (plus 127489 engine
frames) must not corrupt each other's single-stream reassembly.

Build gate: the whole module compiles only with `-DENABLE_NMEA2000`; without
it `nmea2000::setup()` starts a no-op worker task
(`src/source_nmea2000.cpp`, `#else` branch) and logs "not compiled in". As of
this working tree the flag is defined only in the Waveshare Touch LCD 4 env
blocks of `platformio.ini` (the dev env and `release_waveshare_common`) — the
base Sunton `esp32-4848s040` env does **not** set it, so the default board
ships without NMEA2000 decode compiled in. At
runtime it is further gated by an NVS-persisted `enabled` flag (default
**off**), independent of the compile flag. The CAN transceiver is held in
`TWAI_MODE_LISTEN_ONLY` unconditionally; a `tx_enabled` NVS flag exists as a
future transmit gate but **no transmit code path is wired** — the comment in
`decode_id`'s caller is explicit that this only prepares for a future
autopilot/NMEA transmit backend.

| PGN | Name | Fields decoded | Published to | Frame type | Notes |
|---|---|---|---|---|---|
| 127250 | Vessel Heading | heading, deviation, variation, reference byte | `heading_true_rad` (ref 0) or `heading_mag_rad` (ref 1) + derived `heading_true_rad` = mag+variation when only magnetic is present; `variation_rad` | single | Reference byte honored explicitly (labelled "BUG-2" in comments — an earlier version conflated true/magnetic). Reference values other than 0/1 drop the heading entirely (variation may still publish). |
| 127251 | Rate of Turn | rate (s32 × 3.125e-8 rad/s) | `rate_of_turn_radps` | single | +ve = turning to starboard. |
| 127257 | Attitude | yaw, pitch, roll (s16 × 1e-4 rad) | `roll_rad`, `pitch_rad` | single | Yaw is decoded but never published — it duplicates heading. |
| 127245 | Rudder | instance, angle order, position (s16 × 1e-4 rad) | `rudder_angle_rad` (position only, wrapped to ±π) | single | `angle_order_rad` is decoded but not published (no commanded-rudder field). |
| 127488 | Engine Parameters Rapid | instance, RPM (u16 × 0.25), boost pressure | `engine_rev_hz` (RPM/60) | single | **Instance 0 only** — treated as the primary engine, matching 127489. Boost pressure decoded, not published (no field). |
| 127489 | Engine Parameters Dynamic | instance, oil pressure, oil temp, coolant temp, alternator V, fuel rate, total hours, coolant/fuel pressure | `engine_oil_pressure_pa`, `engine_coolant_temp_k`, `engine_fuel_rate_m3s`, `engine_hours_s` | **fast packet** (26-byte payload) | **Instance 0 only.** Oil temp and alternator voltage are decoded (`EngineDynamic127489`) but have no `Snapshot` field and are dropped. |
| 127508 | Battery Status | voltage (u16 × 0.01 V) | `battery_v` | single | Decoded inline in `source_nmea2000.cpp` (not in `n2k_decode.h`) — no dedicated struct. |
| 128267 | Water Depth | depth (u32 × 0.01 m) | `depth_m` | single | Decoded inline; below-transducer only (no offset/keel field consumed). |
| 129025 | Position, Rapid Update | lat/lon (s32 × 1e-7 deg) | `lat_deg`, `lon_deg` | single | Decoded inline. |
| 129026 | COG & SOG, Rapid Update | COG (u16 × 1e-4 rad), SOG (u16 × 0.01 m/s) | `cog_true_rad`, `sog_mps` | single | Decoded inline; COG is always treated as true (no reference byte consulted). |
| 129038 | AIS Class A Position Report | MMSI, lat/lon, SOG, COG, true heading | `ais::store().upsert_position(..., VesselClass::ClassA, ...)` — **never** `boat::Snapshot` | **fast packet** | Reassembled per `(pgn, src_addr)` via the multi-stream pool so concurrent targets don't clobber each other. |
| 129039 | AIS Class B Position Report | same layout as 129038 | `ais::store().upsert_position(..., VesselClass::ClassB, ...)` | **fast packet** | Same decoder as 129038 (`decode_ais_position`); heading often unavailable on Class B. |
| 129794 | AIS Class A Static and Voyage Data | MMSI, name (20 ASCII, `@`/space/0xFF-trimmed) | `ais::store().upsert_static(mmsi, name, VesselClass::ClassA, ...)` | **fast packet** (75-byte payload) | IMO number and callsign are consumed by the payload but not decoded into the `AisStatic129794` struct (name only). |
| 130306 | Wind Data | speed (u16 × 0.01 m/s), angle (u16 × 1e-4 rad), reference (3-bit) | ref 2 → `aws_mps`/`awa_rad`; ref 0 or 3 → `tws_mps`/`twa_rad` | single | Labelled "BUG-3" in comments: ref 1 (magnetic ground wind direction) and reserved refs are explicitly **dropped and counted** (`s_wind_ref_dropped`), never folded into TWA. |
| 127237 | Heading/Track Control | commanded heading-to-steer (u16 × 1e-4 rad, bytes 4-5) | `autopilot_target_rad` | single | This is a **standard** NMEA2000 PGN (not manufacturer-proprietary), even though it lives in the source's "Raymarine pilot PGNs" code block — only the heading-to-steer subset is consumed. |
| 65360 | Raymarine Pilot Locked Heading | locked heading (u16 × 1e-4 rad, bytes 2-3) | `autopilot_target_rad` | single, **proprietary** | Manufacturer PGN (Raymarine/Raymarine-compatible); decoded from public protocol references only (no GPL source imported, per `docs/specs/12-nmea2000-and-visual-adoption.md`). |
| 65379 | Raymarine Pilot Mode/Submode | mode enum (u16, bytes 4-5) | `boat::publish_autopilot_state()` → `"standby"`/`"auto"`/`"wind"`/`"track"` | single, **proprietary** | Unrecognized mode values are silently ignored (no state published). |
| 65288 | Raymarine Seatalk Alarm | status, alarm id, alarm group | `notif::store().upsert("seatalk.alarm.<group>.<id>", ...)` | single, **proprietary** | status 1 = active (sound+visual alarm); status 2 = met-but-silenced-at-source (visual only, upserted **pre-acknowledged**); status 0 = condition cleared (upserts `State::Normal`, which removes the entry). |
| 65345 | Raymarine Pilot Wind Angle | locked wind angle (u16 × 1e-4 rad, bytes 2-3) | `twa_rad` (piggybacked) | single, **proprietary** | No dedicated field — reuses `twa_rad` as a "target wind angle" hint; will read as ordinary TWA to any consumer that doesn't know it's a pilot lock. |

Any PGN not in the switch increments `s_pgns_unknown` and is otherwise
ignored (`decode_pgn` default case).

## 2. NMEA 0183 sentences

Dispatch: `nmea0183::parse_sentence()` in `src/nmea0183_parser.cpp` (string
match on the 3-letter sentence id after checksum verification). Field
semantics: `include/nmea0183.h` (`FieldKind` enum). The publish map from
`FieldKind` to `boat::Snapshot` lives in `src/source_nmea_wifi.cpp`
(`on_sentence`). Transport: `nmea_wifi::worker()` in the same file — **UDP**
listening on port 10110 by default (listen-only, non-blocking, matches the
common NMEA0183-over-WiFi broadcast setup), or **TCP** client mode opt-in via
`nmea-wifi tcp <host> <port>`.

| Sentence | Fields used | Published to | Notes |
|---|---|---|---|
| RMC | status, lat/lon, SOG (kn), COG (true), magnetic variation + E/W | `lat_deg`, `lon_deg`, `sog_mps`, `cog_true_rad`, `variation_rad` | Status must be `A` (active); `V` (warning) sentences are dropped entirely (`parse_rmc` returns early). Variation is negated for `W`. |
| GGA | lat/lon, fix quality flag | `lat_deg`, `lon_deg` | **Fix-flag gate**: `fix == 0` (no fix) drops the whole sentence, including lat/lon — a stale/invalid GGA never republishes position. |
| VTG | COG (true), SOG (kn) | `cog_true_rad`, `sog_mps` | Magnetic COG field is ignored. |
| VHW | heading true, heading magnetic, STW (kn) | `heading_true_rad`, `heading_mag_rad`, `stw_mps` | Publishes **both** heading forms from one sentence; this makes `has_true_hdg` true in `on_sentence`, which suppresses the mag→true derivation for the magnetic value in the *same* delta (VHW already carries a real true heading, so no need to derive one). |
| HDT | heading true | `heading_true_rad` | — |
| HDG | heading magnetic, deviation, variation + E/W | `heading_mag_rad`, `variation_rad` | Deviation is parsed by the sentence grammar comment but not pushed as a field (no `FieldKind` for it). Variation negated for `W`. |
| MWV | wind angle (0-360, wrapped to signed ±180), reference (R=relative/T=true), speed, unit (N/K/M), status | `awa_rad`/`aws_mps` (relative) or `twa_rad`/`tws_mps` (`T`) | Status must be `A` (valid); `V` drops the sentence. **Unit handling**: `N` = already knots (no-op), `K` = km/h × 0.539957 → kn, `M` = m/s × 1.943844 → kn; unrecognized unit chars force speed to NaN. The parser's knot value is then converted device-side to m/s (`units::kn_to_mps`) for the SI `Snapshot` field. |
| DPT | depth below transducer | `depth_m` | Only the first field (depth) is used; offset-to-transducer and max-range fields are ignored. |
| DBT | depth in feet / **meters** / fathoms | `depth_m` | Uses only the meters field (`t[2]`); feet (`t[0]`) and fathoms (`t[4]`) are ignored. |
| MTW | water temperature (°C) | `water_temp_k` (via `units::c_to_k`) | — |
| XTE | status ×2, value, L/R, unit (N/K) | `xte_m` (via `units::nm_to_m`) | First status field must be `A`; `V` drops the sentence. `K` (km) is converted to nm by ×0.539957 (same nm/km factor as MWV's K case, correctly applied here to distance rather than speed). `L` (left of track) negates the value. |
| BWC | UTC, lat/lon (unused), bearing-to-waypoint true + magnetic, distance-to-waypoint (nm) | `btw_rad`, `dtw_m` (via `units::nm_to_m`) | Only the **true** bearing is consumed; magnetic bearing to waypoint is ignored. Distance is assumed to already be nm — unlike XTE, there is no unit-char branch on the distance field. |

Sentences with a bad checksum or an unrecognized 3-letter id return
`ok = false` from `parse_sentence` and are silently counted as bad
(`s_sentences_bad` in `source_nmea_wifi.cpp`); no partial fields are
published for those.

## 3. SignalK paths

Delta application: `sk::applyValue()` (self-vessel numeric/typed paths) and
`sk::applyDelta()` (context routing: self vs. AIS vs. notifications) in
`src/signalk_parser.cpp`. Subscription lists: `FULL_PATHS` and
`BASELINE_PATHS` in `src/signalk.cpp`. The canonical path↔`MetricSource`
bridge used by the classic/legacy renderer is `path_to_source()`
(`src/metric_source_map.cpp`) — a second, independent listing of most of the
same paths, useful as a cross-check.

### 3a. Typed self-vessel paths

Every row below is numeric SI on `boat::View`/`boat::Snapshot`
(`include/boat_data.h`) unless noted. "Subscribed by default" means the path
is a literal entry in `FULL_PATHS` (sent whenever a screen's desired-path set
falls back to the full legacy list, e.g. before the first screen-change
callback) or `BASELINE_PATHS` (sent on **every** screen, always). Paths
accepted by the parser but not subscribed by default only populate the field
if some other producer (a screen's own `collect_paths`, or a manually
authored MIDL binding) requests them.

| SignalK path | `boat::View` field | `FieldId` | Unit conversion | Subscribed by default? |
|---|---|---|---|---|
| `navigation.position` (`{latitude,longitude}` object) | `lat`, `lon` | `Lat`, `Lon` | none (deg) | yes |
| `navigation.speedOverGround` | `sog` | `Sog` | none (m/s, SI) | yes |
| `navigation.speedThroughWater` | `stw` | `Stw` | none (m/s) | yes |
| `navigation.courseOverGroundTrue` | `cogTrue` | `CogTrue` | none (rad) | yes |
| `navigation.headingTrue` | `headingTrue` | `HeadingTrue` | none (rad) | yes |
| `environment.wind.angleApparent` | `awa` | `Awa` | none (rad) | yes |
| `environment.wind.speedApparent` | `aws` | `Aws` | none (m/s) | yes |
| `environment.wind.angleTrueWater` **or** `environment.wind.angleTrueGround` | `twa` | `Twa` | none (rad) | only `angleTrueWater` |
| `environment.wind.speedTrue` | `tws` | `Tws` | none (m/s) | yes |
| `environment.depth.belowTransducer` **or** `environment.depth.belowSurface` | `depth` | `Depth` | none (m) | only `belowTransducer` |
| `environment.depth.belowKeel` | `depthKeel` | `DepthKeel` | none (m) | yes |
| `environment.water.temperature` | `waterTemp` | `WaterTemp` | none (K, SI) | yes |
| `environment.outside.temperature` | `outsideTemp` | `OutsideTemp` | none (K) | yes |
| `environment.outside.pressure` | `outsidePressure` | `OutsidePressure` | none (Pa) | yes |
| `environment.outside.humidity` **or** `environment.outside.relativeHumidity` | `humidity` | `Humidity` | none (ratio 0..1) | yes (both spellings) |
| `navigation.attitude` (`{roll,pitch}` object; yaw ignored) | `roll`, `pitch` | `Roll`, `Pitch` | none (rad) | yes (object path only) |
| `navigation.attitude.roll` / `.pitch` (leaf alternative) | `roll` / `pitch` | `Roll` / `Pitch` | none (rad) | no (leaf form not in `FULL_PATHS`) |
| `navigation.rateOfTurn` | `rateOfTurn` | `RateOfTurn` | none (rad/s) | yes |
| `navigation.trip.log` | `tripLog` | `TripLog` | none (m) | yes |
| `navigation.log` | `totalLog` | `TotalLog` | none (m) | yes |
| `navigation.headingMagnetic` | `headingMag` | `HeadingMag` | none (rad) | yes |
| `navigation.magneticVariation` | `variation` | `Variation` | none (rad, +E/-W) | yes |
| `electrical.batteries.<inst>.voltage` | `battVoltage` | `BattVoltage` | none (V) | only instance `house` |
| `electrical.batteries.<inst>.stateOfCharge` | `battSoc` | `BattSoc` | none (ratio 0..1) | only instance `house` |
| `electrical.batteries.<inst>.current` | `battCurrent` | `BattCurrent` | none (A, signed) | only instance `house` |
| `electrical.batteries.<inst>.temperature` | `battTemp` | `BattTemp` | none (K) | only instance `house` |
| `propulsion.<inst>.revolutions` | `engineRevs` | `EngineRpm` | none (Hz, SI; display ×60 for RPM) | only instance `main` |
| `propulsion.<inst>.temperature` | `engineCoolantTemp` | `EngineCoolantTemp` | none (K) | only instance `main` |
| `propulsion.<inst>.oilPressure` | `engineOilPressure` | `EngineOilPressure` | none (Pa) | only instance `main` |
| `propulsion.<inst>.fuel.rate` | `engineFuelRate` | `EngineFuelRate` | none (m³/s) | only instance `main` |
| `propulsion.<inst>.runTime` | `engineHours` | `EngineHours` | none (s; display ÷3600 for hours) | only instance `main` |
| `tanks.fuel.<inst>.currentLevel` | `tankFuel` | `TankFuel` | none (ratio 0..1) | only instance `0` |
| `tanks.freshWater.<inst>.currentLevel` | `tankWater` | `TankWater` | none (ratio 0..1) | only instance `0` |
| `navigation.courseRhumbline.crossTrackError` **or** `courseGreatCircle.crossTrackError` | `xte` | `Xte` | none (m, signed) | only `courseRhumbline` |
| `navigation.courseRhumbline.bearingTrackTrue` **or** `courseGreatCircle.bearingTrackTrue` | `cts` | `Cts` | none (rad) | only `courseRhumbline` |
| `navigation.courseRhumbline.nextPoint.bearingTrue` **or** `courseGreatCircle.nextPoint.bearingTrue` | `btw` | `Btw` | none (rad) | only `courseRhumbline` |
| `navigation.courseRhumbline.nextPoint.distance` **or** `courseGreatCircle.nextPoint.distance` | `dtw` | `Dtw` | none (m) | only `courseRhumbline` |
| `navigation.courseRhumbline.velocityMadeGood` **or** `courseGreatCircle.velocityMadeGood` | `vmg` (waypoint VMG) | `Vmg` | none (m/s) | only `courseRhumbline` |
| `performance.velocityMadeGood` | `vmgWind` (wind/polar VMG — **distinct** field from `vmg`) | `VmgWind` | none (m/s) | yes |
| `performance.beatAngle` | `beatAngle` | `BeatAngle` | none (rad) | yes |
| `performance.gybeAngle` | `gybeAngle` | `GybeAngle` | none (rad) | yes |
| `steering.autopilot.target.headingTrue` | `apTargetHdg` | `ApTargetHdg` | none (rad) | yes |
| `steering.rudderAngle` | `rudder` | `Rudder` | none (rad, +stbd helm) | yes |
| `environment.current.setTrue` **or** `current.drift.setTrue` | `currentSetTrue` | `CurrentSet` | none (rad) | only `current.setTrue` |
| `environment.current.drift` **or** `current.speed` | `currentDrift` | `CurrentDrift` | none (m/s) | only `current.drift` |
| `steering.autopilot.state` | `apState` (char[16], **string**, not numeric) | `ApState` | n/a | yes — via `BASELINE_PATHS`, not `FULL_PATHS` |
| `electrical.batteries.<inst>.*` (any suffix, any instance) | routed by `endsWith()` regardless of the exact instance segment — e.g. `electrical.batteries.house.capacity.stateOfCharge` still lands on `battSoc` | — | — | only the exact literal paths above are subscribed; non-canonical instances/suffixes only populate if independently subscribed |

**Contradiction found in source**: the `FULL_PATHS` comment in
`src/signalk.cpp` above the `"performance.velocityMadeGood"` entry says "the
parser maps either onto `sk::s_parsed.vmg`" — but `applyValue()` in
`src/signalk_parser.cpp` routes `performance.velocityMadeGood` to the
**separate** `vmgWind` field (`FI::VmgWind`), not `vmg`. The code comment is
stale; the actual behavior (two distinct VMG fields — waypoint VMG vs.
wind/polar VMG) matches the `boat_data.h` field documentation, not the
`signalk.cpp` comment. Not fixed here (out of scope — `docs/` only).

### 3b. Special streams

| Path/context | Semantics | Subscribed |
|---|---|---|
| `notifications.*` | Object `{state, message, method[]}` per alarm suffix. `sk::apply_notification()` (`src/signalk_parser.cpp`) routes to `notif::store().upsert()` — **never** a `boat::View` field. `state` maps via `notif::state_from_string()`: `"normal"`/`"nominal"` → `Normal` (clears/removes the entry); `"alert"`, `"warn"`/`"warning"`, `"alarm"`, `"emergency"` map directly; any other/unknown string conservatively maps to `Alert` (never silently dropped). A `null` value also clears. `method[]` entries `"visual"`/`"sound"` set `notif::METHOD_VISUAL`/`METHOD_SOUND` bit flags. | Yes, always — `BASELINE_PATHS` (every screen) **and** `FULL_PATHS`. |
| `vessels.*` AIS (3 paths: `navigation.position`, `navigation.speedOverGround`, `navigation.courseOverGroundTrue`) | Separate SignalK `subscribe` message with `"context": "vessels.*"` (`send_ais_subscribe()`), kept apart from the self-context per-screen diff/unsubscribe machinery. 1 Hz (`period`/`minPeriod` = 1000 ms). Non-self deltas are routed by `apply_ais_value()` into `ais::store()`, keyed by MMSI parsed from the delta's `context` (`context_mmsi()`); contexts that aren't an `mmsi:` URN (uuid vessels, shore stations) are dropped. | Runtime-toggleable, NVS-persisted, **default ON** (`sk-ais on\|off`; takes effect on next reconnect, or immediately via `sk-reconnect`). |
| `network.yeyboats.configPush` | Not a boat metric. `onText()` (`src/signalk.cpp`) substring-scans every WS frame for `"configPush"` + this device's id; on a hit it calls `manager::request_config_fetch()` for immediate pull instead of waiting for the poll interval. | Yes — in both `FULL_PATHS` and `BASELINE_PATHS`. |

### 3c. Dynamic any-path fallback

Every numeric value in a **self-vessel** delta — whether or not `applyValue()`
recognized the path — is mirrored into `sk::dynamicStore()` (a
`sk::PathStore`, `include/path_store.h`) via `dyn->set(p, val.as<double>())`
in `apply_delta_impl` (`src/signalk_parser.cpp`). This lets an authored MIDL
or layout-editor binding reference **any** numeric SignalK path without a
firmware recompile.

Resolution order for a bound path (`widget_data::resolve_numeric()`,
`src/widget_data_resolver.cpp`):
1. A small alias table (`boat.sog`, `boat.headingTrue`, ... — local
   shorthand names).
2. The same table's raw-SignalK-path entries (the ones also listed in 3a).
3. `sk::PathStore::get()` — the dynamic fallback, for anything steps 1-2
   miss.

`format.unit` on the MIDL element controls the display-unit conversion
applied by the painter (`midl_render_apply.cpp`, `resolve_numeric_binding()`
docstring: "the painter applies the format.unit conversion"). Subscription
for these dynamic paths rides the normal per-screen `collect_paths()`
mechanism — `ui::layouts::collect_paths()` walks a built screen's
`MetricBinding`s (including ones whose raw path was retained because
`path_to_source()` missed — see the "Dynamic-path fallback" comment in
`include/midl_render.h`) into the `SubscriptionSet` passed to
`sk::setDesiredPaths()`.

**Gap found in source**: only *numeric* values are captured
(`apply_delta_impl` gates the `dyn->set()` mirror on `isNumeric(val)`).
**String**-valued SignalK paths have no equivalent dynamic store —
`widget_data::resolve_string()` (`src/widget_data_resolver.cpp`) recognizes
exactly one path, `steering.autopilot.state` (alias `boat.autopilotState`).
The baked-in MIDL "Anchor" gallery screen (§4 below) binds a `text` element to
`navigation.state`, which is a SignalK **string** enum
(`anchored`/`sailing`/`motoring`/...) — that binding has no resolver and will
not render a value. **Verify on hardware.**

## 4. Built-in dashboard usage

### MIDL library gallery (6 screens, `include/midl_demo_doc.h`)

Baked JSON (`midl::demo::SQUARE_480_JSON`), 1:1 with the MIDL web demo's
"standard layout library". Rendered via `midl::render::apply_all()`
(`src/midl_render_apply.cpp`); reachable from the `midl-render` console
command and the `YEYBOATS_MIDL_ONLY` boot mode.

| Screen (id) | Elements → SignalK paths |
|---|---|
| Wind (`dash`) | windrose ← `environment.wind.speedApparent` + `.angleApparent`; single-value SOG ← `navigation.speedOverGround`; compass HDG ← `navigation.headingTrue` |
| Course (`nav`) | single-value DTW ← `navigation.courseGreatCircle.nextPoint.distance`; single-value BTW ← `.nextPoint.bearingTrue`; compass COG ← `navigation.courseOverGroundTrue`; bar XTE ← `navigation.courseGreatCircle.crossTrackError` (range ±0.2 nm) |
| Engine | gauge RPM ← `propulsion.main.revolutions` (0-3600, zoned); gauge COOLANT ← `propulsion.main.temperature` (0-130°C, zoned); bar FUEL ← `tanks.fuel.0.currentLevel` (0-100%, zoned); single-value OIL ← `propulsion.main.oilPressure` (bar) |
| Power | bar SOC ← `electrical.batteries.house.capacity.stateOfCharge` (non-canonical path — see note below); gauge VOLTS ← `electrical.batteries.house.voltage`; trend SOLAR ← `electrical.solar.0.panelPower` (no typed field anywhere — dynamic-path only); single-value CURRENT ← `electrical.batteries.house.current` |
| Race | windrose ← `environment.wind.speedApparent`/`.angleApparent`; trend VMG ← `performance.velocityMadeGood` (this is the wind/polar VMG field, `vmgWind`, per §3a — the element is still labelled "VMG"); autopilot PILOT ← `steering.autopilot.state`; button TACK — command action `steering.autopilot.tack`, no data path |
| Anchor | single-value DEPTH ← `environment.depth.belowTransducer`; compass HEADING ← `navigation.headingTrue`; text STATUS ← `navigation.state` (**string path with no resolver** — see §3c gap) |

Note on Power/SOC: `electrical.batteries.house.capacity.stateOfCharge` is
**not** the exact literal string `path_to_source()` or the typed
`FULL_PATHS` subscription recognize, but `applyValue()`'s `endsWith(".stateOfCharge")`
prefix/suffix match still routes it onto the typed `battSoc` field for
*rendering* — and it also lands under its own literal key in the dynamic
`PathStore`. Net effect: the value renders correctly on hardware as long as
the screen's own `collect_paths()` subscribes that literal path (it does, via
the retained raw-path fallback in §3c) — but this path is easy to miss when
auditing `FULL_PATHS` alone, since it's absent from that list verbatim.

### Classic/legacy grid screens (`src/ui/screen_*.cpp`)

Fixed-layout screens built from `ui::layouts::MetricSource` tiles
(`include/ui_layouts_types.h`) or direct `boat::View` field reads, each with
its own `collect_paths()` registered via `ui::set_screen_collect_paths()`.

| Screen | Paths/metrics shown |
|---|---|
| Dashboard (`screen_dashboard.cpp`) | `MetricSource::`{`AWA_deg`, `AWS_kn`, `BatterySOC_pct`, `Depth_m`, `SOG_kn`, `TWA_deg`, `TWS_kn`} |
| Navigation (`screen_nav.cpp`) | `MetricSource::`{`COG_deg`, `CTS_deg`, `HDG_deg`, `Position`, `SOG_kn`} |
| Route (`screen_route.cpp`) | `MetricSource::`{`BTW_deg`, `CTS_deg`, `DTW`, `VMG_kn`, `XTE`} |
| Depth (`screen_depth.cpp`) | `MetricSource::`{`DepthKeel_m`, `SOG_kn`, `TWA_deg`, `WaterTemp_C`} |
| Steering (`screen_steering.cpp`) | `MetricSource::`{`CTS_deg`, `HDG_deg`, `Rudder_deg`, `VMG_kn`, `XTE`} |
| Demo grid (`screen_demo_grid.cpp`) | `MetricSource::`{`AWS_kn`, `BatteryV`, `Depth_m`, `SOG_kn`} — widget-preview gallery, not a live nav screen |
| Wind / Wind classic (`screen_wind.cpp`, `screen_wind_classic.cpp`) | direct `boat::View` reads: `awa`, `aws`, `twa`, `tws`, `headingTrue`, `btw`, `currentSetTrue`, `currentDrift` (+ `sog`, `stw` in the classic variant) |
| Wind-steer (`screen_wind_steer.cpp`) | direct reads: `awa`, `aws`, `twa`, `tws`, `headingTrue`, `cogTrue`, `cts`, `xte`, `beatAngle`, `gybeAngle`, `apState` (semicircular `WindSteer` dial) |
| Autopilot (`screen_autopilot.cpp`) | direct reads: `apState`, `apTargetHdg`, `awa`, `aws`, `cogTrue`, `cts`, `depth`, `headingTrue`, `sog`, `stw`, `xte` |
| Trip (`screen_trip.cpp`) | direct read: `sog` only — **locally integrates** trip/total distance from SOG over time rather than consuming the `navigation.trip.log`/`navigation.log` SignalK paths (which §3a shows the parser fully supports as `TripLog_nm`/`Log_nm`) |
| Status (`screen_status.cpp`) | direct reads: `battSoc`, `battVoltage`, `tankFuel`, `tankWater` |

**Supported but not on any default screen**: grepping every `ui/*.cpp` and
`src/*.cpp` for the "coverage wave" `MetricSource` values (`Roll_deg`,
`Pitch_deg`, `OutsideTemp_C`, `OutsidePressure_hPa`, `Humidity_pct`,
`ROT_degmin`, `TripLog_nm`, `Log_nm`, `BattCurrent_A`, `BattTemp_C`,
`EngineRpm`, `EngineCoolant_C`, `EngineOilP_bar`, `EngineFuelRate_lph`,
`HDGm_deg`, `Variation_deg`, `EngineHours_h`, `VMGwind_kn`, `STW_kn`) finds
them only in the generic renderer plumbing (`src/metric_source_map.cpp`,
`src/metric_value.cpp`, `src/layout_renderer.cpp`, `src/ui/ui_layouts.cpp`) —
none of the fixed classic screens above bind them directly. They render only
through a custom-authored layout (layout editor / manager-pushed config) or a
MIDL screen that binds the underlying path/alias explicitly. This includes
attitude (roll/pitch/clinometer outside `screen_wind_steer`'s indirect use),
outside temp/pressure/humidity, battery current/temp, and all engine fields
outside the MIDL Engine gallery screen.

## 5. Test coverage

Host test suites live under `test/` and run via `pio test -e native`. The
native `build_src_filter` in `platformio.ini` is broader than
`instruments/CLAUDE.md`'s summary ("Builds only `signalk_parser.cpp` +
`layout.cpp`") describes — it now also builds `boat_data.cpp`,
`nmea0183_parser.cpp`, `path_store.cpp`, `widget_data_resolver.cpp`,
`metric_source_map.cpp`, `metric_value.cpp`, `midl_render.cpp`, and more; that
CLAUDE.md line is stale.

| Area | Coverage | Notes |
|---|---|---|
| NMEA0183 sentence parsing (§2) | **Yes** | `test/test_nmea0183/test_nmea0183.cpp` exercises the sentence grammar end to end (checksum, all 11 sentence types, unit conversions). |
| NMEA2000 PGN payload decode (`n2k_decode.h`) (§1) | **Yes, for the pure decoders** | Same file, `test/test_nmea0183/test_nmea0183.cpp`, tests every `n2k::decode_*` function including the multi-stream fast-packet pool and AIS/Seatalk-alarm decode. |
| NMEA2000 device adapter (`decode_pgn` dispatch, TWAI worker, `source_nmea2000.cpp`) | **No** | `source_nmea2000.cpp` is not in the native `build_src_filter` (it needs the TWAI driver / `ENABLE_NMEA2000`); the PGN→`boat::publish` wiring and the Raymarine-mode-string mapping are untested on the host. |
| SignalK delta parsing incl. notifications/AIS context routing (§3) | **Yes** | `test/test_parser/test_parser.cpp` (904 lines) covers `applyValue`/`applyDelta`, self-vs-AIS context routing, notification upsert/clear, and the touched-mask never leaking into AIS/notification paths. |
| `notif::Store` (upsert/ack/expire/evict semantics) | **Yes** | `test/test_notifications/test_notifications.cpp` — 9 cases: severity ordering, ack silencing, expiry, capacity eviction, state-string bridging, truncation safety. `test_notifications` is in `platformio.ini`'s native `test_filter` (the store is header-inline, so no `build_src_filter` entry is needed). |
| `ais::Store` (upsert/dedupe/age-out/eviction) | **Yes** | `test/test_ais/test_ais.cpp` — 7 cases: position/static merge, MMSI dedupe with NaN-field merge, age-out, snapshot filtering, capacity eviction. `test_ais` is in the native `test_filter`; store is header-inline like `notif::Store`. |
| `boat::Snapshot` fusion (priority/staleness arbitration, touched-mask gating) | **Yes** | `test/test_boat_data/test_boat_data.cpp` — priority ordering, freshness windows, per-field touched-mask gating, autopilot-state publish, engine-hours compose. |
| Display-unit conversion per `MetricSource` (§4) | **Yes** | `test/test_metric_value/test_metric_value.cpp` — covers every "coverage wave" source's unit conversion and gauge-fraction mapping, including several sources no default screen currently uses. |

## See also

- `docs/specs/12-nmea2000-and-visual-adoption.md` — the NMEA2000 adoption
  spec (licensing boundary, why proprietary PGNs are reimplemented from
  public protocol references only).
- `docs/specs/15-signalk-nmea0183-wifi.md` — how the lab SignalK server emits
  the NMEA0183-over-WiFi stream this firmware consumes.
- `docs/specs/19-device-display-widget-management.md` — the layout/MIDL
  binding model referenced in §3c/§4.
