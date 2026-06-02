# Implementation Plan

This plan turns the roadmap into executable work packages. It is intentionally
ordered so each step leaves the project in a testable state before the next
layer depends on it.

## Principles

- Keep the display useful without SignalK manager connectivity.
- Prefer pull-based device behavior: the firmware registers, fetches config,
  polls commands, and reports status.
- Treat the SignalK plugin as the control plane, not the renderer.
- Add tests at the same boundary as each feature: plugin unit tests for plugin
  state, native firmware tests for parsing/apply logic, system tests for HTTP
  contracts, and hardware tests for display/touch/OTA behavior.
- Keep release artifacts reproducible from tags.

## Phase 1 - Firmware Manager Client MVP

Goal: make a real flashed display visible and controllable by the SignalK
manager without breaking standalone operation.

Tasks:

1. Manager endpoint model
   - Add a single runtime model for manager host, port, base path, TLS flag,
     token state, last discovery method, heartbeat interval, and command poll
     interval.
   - Persist only stable user/device fields in NVS.
   - Acceptance: `manager-status` shows endpoint, token state, discovery
     state, last HTTP result, config hash, and pending command count.
   - Tests: native unit tests for default state, persistence migration, and
     redacted token display.

2. Manager discovery
   - Resolve manager endpoint from stored config, mDNS
     `_espdisp-mgmt._tcp.local`, SignalK/plugin well-known metadata, then
     manual configuration.
   - Preserve explicit manual config over auto-discovery.
   - Acceptance: `manager-discover` can populate the endpoint without changing
     `sk-host auto`.
   - Tests: host tests for endpoint priority and malformed metadata handling;
     system discovery test against the SignalK lab stack.

3. Registration
   - POST stable identity, board/display/touch capabilities, firmware build
     metadata, network identity, and current auth state.
   - Accept heartbeat and command polling intervals from the server.
   - Store a returned device token only after a successful provisioning
     response.
   - Acceptance: a flashed device appears as registered or claimable in the
     SignalK plugin device list.
   - Tests: mock server registration contract test; plugin test for capability
     ingestion.

4. Heartbeat/status
   - POST network, SignalK, UI, touch, memory, firmware, OTA, config, and
     recent-error status on a bounded cadence.
   - Use heartbeat responses to learn desired config hash and pending command
     count.
   - Acceptance: SignalK device detail shows live status and detects stale
     devices.
   - Tests: native payload-builder tests and plugin status reconciliation tests.

5. Config fetch/apply/report
   - GET generated config, validate schema/version/hash, apply in dependency
     order, persist only accepted config, and retain previous config on error.
   - Start with current managed-screen/dashboard MVP, then expand to
     multi-screen layouts and widget variants.
   - Acceptance: changing a dashboard preset in SignalK queues reload, the
     device fetches it, applies it, and reports the matching hash.
   - Tests: native validation tests, plugin mock-firmware e2e, and real-device
     `/api/dashboard/config.json` tests.

6. Command polling and ack
   - Poll `GET /devices/:id/commands`, execute safe v1 commands, and ACK with
     result codes and messages.
   - Initial safe command set: `config.reload`, `layout.reload`, `theme.set`,
     `brightness.set`, `screen.set`, `beep`, `log.level`.
   - Gate destructive or disruptive commands such as `reboot` and
     `firmware.update` behind explicit implementation steps.
   - Acceptance: SignalK command queue moves from pending to delivered to
     acknowledged on a real display.
   - Tests: mock command server and plugin command lifecycle tests.

## Phase 2 - Provisioning And Security

Goal: make device management safe enough for a trusted boat LAN and explicit
about what is still unsafe.

Tasks:

1. Token storage hardening in the plugin
   - Status: implemented for device tokens and provisioning tokens.
   - Store token hashes only.
   - Return tokens only at creation/rotation time.
   - Audit create, rotate, revoke, and failed auth events.
   - Acceptance: registry exports never contain reusable device tokens.
   - Tests: plugin auth tests and export redaction tests.

2. Claim/provisioning flow
   - Support expiring setup tokens or signed claim payloads.
   - Allow discovered devices to be claimed from the SignalK UI.
   - Persist provisioned state on the device.
   - Acceptance: an unclaimed device can be claimed once, then uses a
     per-device token.
   - Tests: plugin claim flow tests and firmware provisioning contract tests.

3. Local device web/BLE write protection
   - Keep read-only diagnostics available.
   - Require local auth, setup mode, or manager authorization for persistent
     writes after claim.
   - `/api/security` must report the effective model.
   - Acceptance: provisioned devices reject unauthenticated write requests.
   - Tests: real-device and system tests for web auth, BLE config policy, and
     `/api/security`.

4. Threat model documentation
   - Document lab mode, trusted boat LAN, setup AP, and public marina WiFi.
   - Identify which operations are safe, gated, or unsupported in each mode.
   - Acceptance: public docs state the security caveats before install steps.

## Phase 3 - Dashboard Editor And Config Depth

Goal: make dashboard configuration practical for more than a fixed demo layout.

Tasks:

1. Richer SignalK editor controls
   - Add widget add/remove, SignalK path selection, tile ordering, role
     templates, and per-device-size preview.
   - Acceptance: common dashboard changes can be made without JSON/YAML.
   - Tests: Playwright UI tests for create/edit/apply flows.

2. Multi-screen generated configs
   - Generate and validate multiple screens per profile.
   - Respect device capabilities, display geometry, font sizes, and unsupported
     widget fallbacks.
   - Acceptance: wide and square mock devices receive different valid configs
     from the same preset family.
   - Tests: mock firmware capability matrix tests.

3. Device-local configurator polish
   - Improve bench web configurator for dashboard JSON export/import and
     read-only status.
   - Keep YAML on device JSON-compatible unless a bounded parser is justified.
   - Acceptance: a bench user can import/export dashboard config without
     SignalK.
   - Tests: real-device web endpoint tests.

## Phase 4 - Firmware Catalog And Pull OTA

Goal: manage firmware artifacts and updates from SignalK without relying only
on ArduinoOTA.

Tasks:

1. Artifact storage policy
   - Separate metadata from binary storage.
   - Support local lab artifacts and vendor-hosted URLs.
   - Track vendor, product, board, version, channel, checksum, compatibility,
     source URL, provenance, review state, and release notes.
   - Acceptance: stable artifacts cannot be offered without checksum,
     compatibility, and provenance metadata.
   - Tests: plugin artifact validation tests.

2. OTA job lifecycle
   - Create `firmware.update` commands with download URLs and expected
     checksum.
   - Track queued, delivered, downloading, applying, rebooting, confirmed,
     failed, expired, and cancelled states.
   - Acceptance: job state is visible on device and firmware pages.
   - Tests: plugin job lifecycle tests and mock firmware progress tests.

3. Device pull OTA
   - Download with size limits and timeout handling.
   - Verify checksum before install.
   - Install to next partition, reboot, confirm boot, and report failure.
   - Acceptance: hardware test completes successful update and failed-checksum
     rejection.
   - Tests: hardware-in-loop OTA success/failure/interruption tests.

4. Rollback and recovery
   - Define failed boot confirmation behavior.
   - Expose retry/cancel/recover actions in the plugin.
   - Acceptance: interrupted or failed updates do not leave the display
     unmanaged.

## Phase 5 - Discovery And Network Identity

Goal: make devices easy to find, claim, and address on real networks.

Tasks:

1. Manager mDNS advertisement
   - Advertise `_espdisp-mgmt._tcp.local` when the SignalK deployment permits
     multicast.
   - Include protocol, path, auth methods, TLS, SignalK port, and NMEA port.
   - Acceptance: firmware `manager-discover` can find the manager on a LAN.

2. Device advertisement reconciliation
   - Merge UDP announce, mDNS TXT, HTTP discovery fixtures, and registered
     status into one discovery record.
   - Handle stale, duplicate, and conflicting records explicitly.
   - Acceptance: discovery UI shows whether a record is unclaimed, registered,
     stale, duplicate, or conflicting.

3. Hostname/domain management
   - Apply hostname/domain in the right order on device.
   - Restart mDNS after hostname change.
   - Report FQDN, OTA address, and conflict state.
   - Acceptance: SignalK desired hostname matches device reported hostname or
     shows a clear conflict.

4. Network validation matrix
   - Test Docker lab network, normal LAN, setup AP, and constrained marina WiFi
     assumptions.
   - Acceptance: docs list which discovery methods work in each network mode.

## Phase 6 - Testing And CI

Goal: make regressions visible before flashing hardware or publishing releases.

Tasks:

1. SignalK UI e2e coverage
   - Cover discovery, claim, preset assignment, config reload, import/export,
     firmware job creation, and auth failures.

2. Mock firmware matrix
   - Model different sizes, resolutions, widget support, font support,
     firmware versions, auth states, and failure modes.

3. Real-device smoke suite
   - Discover available devices or accept explicit targets.
   - Test `/api/state`, `/api/security`, dashboard config import/export,
     command execution, and SignalK data display.

4. CI release checks
   - Keep firmware build, plugin tests, Playwright tests, plugin tarball, and
     release checksums in CI/release workflows.
   - Add artifact inspection tests for packaged plugin contents.

## Phase 7 - Public Beta

Goal: make the project understandable and testable by external users while
clearly marking production limitations.

Tasks:

1. Fresh-clone verification
   - Validate setup on a clean machine with no cached dependencies.
   - Document exact firmware flash and SignalK plugin install paths from
     release artifacts.

2. Runtime config cleanup
   - Separate committed lab defaults from generated runtime SignalK state.
   - Ensure secrets and local environment files are ignored.

3. Screenshots and demos
   - Regenerate night/day firmware screenshots and SignalK manager screenshots
     from repeatable test sessions.

4. Public announcement package
   - Draft forum post, supported hardware list, known limitations, security
     caveats, roadmap, and contribution entry points.

5. Release readiness checklist
   - Confirm license/commercial-use language, test status, supported hardware,
     install instructions, and artifact reproducibility.

## First Execution Target

The first implementation target is Phase 1, Task 1: Manager endpoint model.
It is the smallest firmware-side foundation that later discovery,
registration, heartbeat, config fetch, and command polling can share.

## Execution Status

- Phase 1, Task 1 is implemented as `manager_endpoint`: a host-testable
  endpoint parser/model with discovery method tracking. Firmware
  `manager-status` and `/api/state` now expose endpoint host, port, TLS flag,
  base path, and discovery method.
- Next task: Phase 1, Task 2, manager discovery. The implementation should
  reuse `manager_endpoint` for mDNS and well-known discovery results instead of
  passing raw URL strings through each path.
