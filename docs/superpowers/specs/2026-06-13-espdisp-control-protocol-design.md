# espdisp Control Protocol — P2P Discovery & Control over IP and BLE

**Date:** 2026-06-13
**Status:** Design (approved; pending spec review)
**Supersedes:** the deferred Phase H of `2026-06-13-waveshare-knob-remote-design.md` (§4.2 SignalK fallback) — replaced by this versioned, multi-transport protocol.

## 1. Goal

Define one **versioned, transport-agnostic control protocol** that lets any *controller*
(the knob, the SignalK plugin, a future phone app) **discover** and **control** any
*target display* directly — over IP (primary) and BLE (fallback) — without routing through
the SignalK manager. The protocol is defined once as a schema and code-generated into a
**C++ library** (firmware: knob + displays) and a **JS/TS library** (plugin + phone), kept in
lockstep by shared conformance fixtures.

Concretely the knob (and later the plugin/phone) discovers displays via **mDNS** (IP) and
**BLE scan**, **attaches** a control session, **switches** the target's active view, and
**detaches**. The target renders a **frame in the controlling controller's color** while
controlled, so a glance shows who is driving it. The system is **many-to-many**: several
controllers may control several targets, and one target may have several concurrent
controllers.

Success criteria:
- A controller discovers and switches a target's view over IP with **no manager** present.
- BLE provides the same control when IP is unavailable, without destabilizing NimBLE.
- One schema generates the C++ and JS libs; both pass the same fixtures; a field added in
  one place can't silently diverge.
- A target shows the controlling controller(s)' color(s); auth (a shared key) gates control
  when configured.

## 2. Protocol definition & codegen

### 2.1 Source of truth
- **`proto/schema/espdisp-control-<major>.schema.json`** — JSON Schema (draft 2020-12) is the
  single source of truth, versioned per major. It defines every message + record type.
- **`proto/fixtures/*.json`** — versioned example messages (valid + a few invalid) used as
  shared conformance vectors by BOTH language suites.

### 2.2 Codegen
- **C++** → `include/proto/` (generated, checked in): a small project-owned generator
  (`proto/gen/gen_cpp.py`) emits ArduinoJson-based `parse()/serialize()` + typed POD structs +
  `constexpr ESPDISP_PROTO_MAJOR/MINOR`. Run via a PlatformIO `extra_script` and a
  `make proto` target; CI verifies the checked-in output is up to date (regenerate + `git diff
  --exit-code`). Hand-rolled (not quicktype) so the output is ArduinoJson-idiomatic and small.
- **JS/TS** → `proto/js/` (an npm package consumed by the plugin/phone): types via
  `json-schema-to-typescript`, runtime validation via `ajv` compiled from the same schema.
- The C++ lib is **pure / host-testable** (no Arduino/LVGL/BLE deps in the parse/serialize
  core — mirrors `signalk_parser.cpp`), so it links in the `native` test env.

### 2.3 Core message/record types (informative — schema is authoritative)
All messages share an envelope `{ "v": "<major>.<minor>", "t": "<type>", ... }`.

- **`DeviceRecord`** (discovery): `deviceId`, `name`, `role` (`display|controller|both`),
  `pv` (protocol version), `board`, `display` (`WxH`, shape), `caps` (touch/ble/...),
  `currentView`, `views: [{id,title}]`, `transports` (`ip`/`ble` + address hints),
  `authRequired` (bool).
- **`Attach`** (controller→target): `controllerId`, `name`, `color` (`#RRGGBB`),
  `key?` (shared-secret), `ttlMs`. → **`AttachAck`**: `sessionId`, `ttlMs`, `accepted|denied`,
  `reason?`, the target's `DeviceRecord`.
- **`Switch`** (controller→target): `sessionId`, `viewId`. → `SwitchAck` `{ok, currentView}`.
- **`Heartbeat`** `{sessionId}` → `HeartbeatAck` `{ok, ttlMs}`.
- **`Detach`** `{sessionId}` → `DetachAck`.
- **`ControlState`** (target→anyone, read): active sessions `[{controllerId,name,color,lastSeen}]`
  + `currentView`. Drives both the target's frame and a controller's "who else is here" view.

### 2.4 Versioning & compatibility
- Every record/message carries `pv`/`v` = `"<major>.<minor>"`. **Same `major` required**;
  a higher `minor` is backward-compatible; **unknown fields are ignored** (forward-compat).
  A `major` mismatch is rejected with `reason:"incompatible_version"` and logged. Discovery
  advertises `pv` so a controller filters incompatible targets before attaching.
- `ESPDISP_PROTO_MAJOR/MINOR` live in the schema `$id`/`const` and flow into both codegens, so
  the constant can't drift from the wire format.

### 2.5 Auth (optional shared key)
- A **system shared key** (a simple password) may be set at setup: NVS (`proto`/`key`) on
  firmware, config on the plugin/phone. When a target has a key set, `Attach`/`Switch` must
  carry a matching `key` or are denied (`reason:"unauthorized"`). Discovery stays open
  (records advertise `authRequired:true`). Stored in NVS and compared on the target
  (`strcmp`) — not hashed at rest, because the key is transmitted in plaintext over the local
  link (LAN/BLE) anyway: this is a local-network shared secret, not internet-grade auth, so
  hashing-at-rest would add no real protection. When no key is set, control is open (today's
  behavior preserved).

## 3. Sessions & the colored "controlled" frame

### 3.1 Session lifecycle (many-to-many, lightweight)
- A controller `Attach`es → target creates a session keyed by `sessionId`, records
  `{controllerId, name, color, lastSeen}`. Multiple concurrent sessions per target are allowed.
- `Switch` applies last-writer-wins to the active view (no exclusive lock). `Heartbeat` (every
  `ttlMs/2`) keeps the session alive; a session with no heartbeat for `ttlMs` (default 10 s) is
  reaped. `Detach` ends it immediately.
- A controller may hold sessions on many targets simultaneously.

### 3.2 The frame (target-side LVGL overlay)
- Rendered on `lv_layer_top()` (survives screen swaps), built/mutated **only on the UI task**
  (memory-trap rule), driven by the session table via the existing `app::Command` queue (the
  BLE/HTTP handlers post; the UI task draws).
- **Multiple concurrent controllers → thin stacked nested borders**, one per active session, in
  each controller's color (outermost = most-recently-active), with the top controller's name on
  a small pill. Single controller → one colored border + name. No active session → no frame.
- Frame width/insets come from the consolidated style config (no inline colors/sizes), and on
  the round knob it follows the inscribed `usable_*`.

### 3.3 Controller color config
- Each controller owns a configurable `color`: knob via NVS (`proto`/`color`) + a console
  command `ctl color #RRGGBB` (routed through `net::dispatchCommand`, serial+BLE); plugin/phone
  via their config. Color travels in `Attach`/`ControlState`.

## 4. Transports

### 4.1 IP (primary) — "the existing REST is the protocol, versioned"
- **Discovery:** firmware **mDNS browse** of `_espdisp._tcp` (displays already advertise it;
  extend TXT with `pv` + `role`). Each hit yields host/IP/port + TXT → a partial `DeviceRecord`;
  a `GET` of the device's control-state/views completes it.
- **Control:** the protocol's IP binding is the **versioned HTTP/JSON surface**, formalizing
  and extending today's endpoints (Arduino `WebServer`, port 80, CORS on):
  - `GET  /api/p2p/device` → `DeviceRecord` (reuses `/api/state` + `/api/screens` data).
  - `POST /api/p2p/attach` `{Attach}` → `AttachAck`.
  - `POST /api/p2p/switch` `{Switch}` → `SwitchAck` (supersedes raw `POST /api/screen/<id>`,
    which stays as an unauthenticated convenience alias).
  - `POST /api/p2p/heartbeat` `{Heartbeat}` / `POST /api/p2p/detach` `{Detach}`.
  - `GET  /api/p2p/state` → `ControlState`.
  All gated by the shared key when configured; envelope carries `v`.
- The knob reuses its existing `HTTPClient` (the manager client plumbing) on the manager worker
  task — never blocking the UI task.

### 4.2 BLE (fallback, no IP) — on-demand central
- **Target side:** an **espdisp Control GATT service** (new UUID block, alongside the existing
  NUS + CONNECTION services) with characteristics: `DEVICE` (read → `DeviceRecord`),
  `CONTROL` (write → `Attach`/`Switch`/`Heartbeat`/`Detach` JSON), `STATE` (read/notify →
  `ControlState`). Reuses the same generated C++ (de)serializers as the HTTP path. **512-byte
  attribute cap** (memory trap): records that exceed it return a summary + the full list comes
  via a follow-up read/`/api/p2p` when IP exists; over BLE-only, the view list falls back to a
  paged read or the standard known-views set.
- **Controller (knob) side — on-demand central:** the knob stays a **peripheral** (its own
  provisioning). It enters **central** mode only when the user opens the BLE-peer flow or IP is
  unavailable: scan for the Control service UUID (+ `pv` in adv/scan-response), connect **one
  peer at a time**, exchange the control messages, **disconnect**. This bounds NimBLE
  internal-SRAM peaks to avoid the documented starvation hang. Requires enabling the NimBLE
  **central role** in `sdkconfig` for the knob env (`CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`,
  `MAX_CONNECTIONS` sized for 1 outbound + the peripheral link) and a heap budget + soak gate.
- BLE is selected only when the peer has no reachable IP (per the IP-primary decision).

### 4.3 Multi-source registry
- `knob_remote` (and the equivalent on other controllers) becomes the **multi-source registry**:
  local + mDNS/IP peers + BLE peers + manager-supplied peers, **deduped by `deviceId`**, each
  entry tagged with its reachable transports and controlled via the best (IP > BLE). The menu
  (Select Display → Select View) is unchanged; entries just carry transport + a small badge.

## 5. Consumers

- **Knob** — controller. IP control over HTTP (manager worker task); on-demand BLE central when
  off-grid. Configurable color + shared key in NVS.
- **Displays** — targets (IP always; BLE Control GATT) that maintain the session table and render
  the colored frame. Remain controller-capable (`role:both`) for future use.
- **SignalK plugin** — **migrated** onto the shared JS protocol lib: discovers via the protocol
  (mDNS/announce) and controls via `Attach`/`Switch`/`Detach`; its current REST is reframed as
  protocol operations. The manager registry persists as an aggregation/naming layer that *speaks
  the protocol* (it can attach as a controller and expose discovered devices). Existing
  `configPush`/registry features are preserved behind the protocol.
- **Phone app** — future; same JS lib; out of scope for v1 but the protocol/auth/color model is
  designed to accommodate it.
- **Headless test harness** — a new firmware build for a bare **ESP32-S3-DevKitC-1** (no display,
  no LVGL/touch): a controller that runs the same shared protocol C++ lib and the same
  controller logic as the knob, but driven by an **automated loop** (discover → attach with a
  configurable color/key → enumerate views → switch through each view → heartbeat → detach →
  assert acks/`ControlState`) plus a console mode for single commands. Logs pass/fail over
  serial and UDP. This is the **on-hardware verification vehicle** in place of the knob: it
  exercises the controller side of both transports (IP and on-demand BLE central) against a real
  target (a Sunton display running the target firmware), so the protocol — including the
  NimBLE-central risk — gets real two-node hardware coverage. The base board for the production
  envs is already `esp32-s3-devkitc-1`, so the harness env is a thin display-less variant.

## 6. Shared libraries (the "common module")

- **`proto/schema/` + `proto/fixtures/`** — source of truth + conformance vectors.
- **C++ lib** (`include/proto/`, `src/proto/`) — generated (de)serializers + version/compat
  helpers + session-table logic where pure; host-tested in `native`. Used by knob AND display
  firmware (no duplication of message handling).
- **JS lib** (`proto/js/`, published as a local npm workspace) — generated types + `ajv`
  validators + the same session/version helpers; consumed by the plugin (and phone).
- **Conformance tests** — `test/test_proto` (Unity, C++) and `proto/js` (node:test) both load
  `proto/fixtures/*.json` and assert identical parse/serialize/version-compat behavior.

## 7. Build order (delivered together as one implementation)

Per the decision to **implement everything together**, the phases below are a build/dependency
ordering inside a single combined plan — not separate releases. Everything lands in one body of
work; the ordering exists only so dependencies are built before their consumers.

1. **Protocol foundation** — schema + codegen (C++ + JS) + both libs + conformance fixtures +
   version/auth/session pure logic. No device behavior change. Fully host/CI-tested.
2. **Test harness firmware** — the headless ESP32-S3-DevKitC-1 controller + automated loop
   (built early, against Phase 1's lib, so it can exercise each subsequent transport as it lands).
3. **IP binding** — displays serve `/api/p2p/*` + session table + the colored frame overlay;
   controllers discover via mDNS browse and control over IP; `knob_remote` multi-source merge.
4. **BLE binding** — Control GATT service on displays; on-demand BLE central on the controller
   (sdkconfig central role, heap budget, soak); BLE fallback discovery + control.
5. **Plugin migration** — plugin adopts the JS lib for discovery + control; reframe REST.
6. **Knob integration** — wire the knob's existing Select-Display/Select-View menu to the
   protocol registry/control (build-verified; hardware-verified only indirectly via the harness,
   which shares the controller path).
7. *(Future, out of scope)* phone app.

## 8. Testing

- **Host (`native`):** `test_proto` — fixtures round-trip, version-compat matrix (same-major
  accept, major-mismatch reject, unknown-field ignore), auth accept/deny, session reap/timeout,
  last-writer-wins, and the frame-model (which colors/sessions are active) as pure logic.
- **JS:** `proto/js` node:test over the same fixtures; plugin tests for discovery + control.
- **Sim (`make sim`):** render the controlled-frame overlay (single + multiple stacked colored
  borders + name pill) at 360 round and 480 square; bounds assertions.
- **Build:** all firmware envs incl. the knob's BLE-central sdkconfig; CI checks generated code
  is up to date (`make proto && git diff --exit-code`).
- **Device (hardware, via the harness):** the **ESP32-S3-DevKitC-1 harness** is the primary
  on-hardware verifier (the knob can't be tested). Two-node setup: harness (controller) ↔ a
  Sunton display running the target firmware. The harness loop asserts: mDNS discovery finds the
  target; IP `attach`→`switch` through every view→`detach` returns correct acks and the target's
  `ControlState`/`currentView` track; the colored frame appears/clears on the target (confirmed
  via `/api/screenshot.png` pulled by the harness, and visually); on-demand BLE central
  discovers + switches when IP is disabled; auth deny/accept with/without the shared key; session
  reaping on dropped heartbeat. NimBLE soak runs with the central role enabled on the harness.
  The knob's own controller path is the same shared code, so it's covered transitively (its
  display wiring is build-verified).

## 9. Memory-trap / risk compliance

- **NimBLE central role is the headline risk** (documented internal-SRAM-starvation hang).
  Mitigations: on-demand central (single outbound connection, torn down after use), a measured
  heap budget, PSRAM for all non-NimBLE large buffers, and a mandatory `espdisp soak` gate with
  the central role enabled before any stability claim. The knob's BLE-central sdkconfig is
  isolated to the knob env.
- **BLE 512-byte attribute cap** — `DeviceRecord`/view-list over BLE summarized/paged.
- **LVGL frame overlay** built/mutated only on the UI task (handlers post `app::Command`s).
- **Large PODs** (session tables, `DeviceRecord` arrays) zeroed in place; never stack temporaries.
- **`NimBLECharacteristic::setValue`** uses the `(uint8_t*, len)` overload.
- Generated C++ stays ArduinoJson-into-PSRAM for large payloads; parse is pure/host-clean.

## 10. Open items to finalize during planning

1. Exact codegen generator details (the `gen_cpp.py` template + the JS `ajv` build wiring) and
   the `make proto` + CI freshness check.
2. The new BLE Control service/characteristic UUIDs (and confirming the `// clang-format off`
   UUID block convention in `board_pins.h`/`ble_config.h`).
3. NimBLE central-role `sdkconfig` deltas + measured heap headroom on the knob (the gating risk).
4. Whether displays advertise `role`/`pv` only in mDNS TXT or also in BLE adv/scan-response
   (size budget in the 31-byte adv vs scan-response).
5. Frame style values (border widths/insets/name-pill) into the consolidated style config.
6. Plugin migration depth in phase 4 (how much of the manager registry is reframed vs retained).
