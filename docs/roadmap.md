# Project Roadmap

This roadmap collects the active firmware, SignalK plugin, device-management,
security, OTA, testing, and release work for the ESP32 boat MFD project.

The project is usable for lab testing and development. It is not yet a
production navigation instrument.

## Current Baseline

Implemented and test-covered today:

- ESP32 display firmware with LVGL screens, touch navigation, WiFi setup, BLE
  diagnostics/config, ArduinoOTA, SignalK WebSocket ingest, NMEA data display,
  autopilot controls, trip metrics, and system/touch diagnostics.
- Repo-owned SignalK lab stack with synthetic data, official NMEA 0183 TCP
  output, official autopilot emulator backend, and app-dock integration.
- Experimental `espdisp-manager` SignalK plugin with device registry,
  discovery fixtures, profiles/presets, generated device configs, structured
  device dashboard configuration UI, command queue, firmware catalog/job model,
  and server-rendered admin pages.
- Multi-device preset management: reusable dashboard presets can be assigned to
  several devices, optionally clear overrides, and queue `config.reload`.
- Display-aware generated configs: display size, widget capabilities, layout
  variants, and supported font sizes are considered before the device receives
  a config.
- Dashboard import/export on SignalK as JSON and YAML, plus matching
  device-local JSON and JSON-compatible YAML endpoints for bench use.
- Device web security reporting through `/api/security`.
- Firmware-side manager client for discovery, registration, token-authenticated
  heartbeats, config fetch/apply, command poll/ack, and pull OTA with
  progress/boot confirmation.
- Plugin registry stores device tokens as hashes; newly issued tokens are only
  returned to the caller at registration, claim, or rotation time.
- Local tests for plugin behavior and host-portable firmware logic.

## Milestones

### M0 - Lab Stack And Core Firmware

Status: mostly complete.

Definition of done:

- Device renders the core navigation screens from SignalK data.
- WiFi setup, touch navigation, day/night theme, diagnostics, and ArduinoOTA
  are available.
- `make demo-up` starts a repeatable SignalK test environment.
- SignalK emits NMEA 0183 over WiFi through the official plugin.
- Autopilot simulator can be queried and commanded through SignalK.

Remaining:

- Keep hardware screenshots and demo artifacts current as the UI changes.
- Continue hardening touch-controller diagnostics and GT911 config dumping on
  real boards.

### M1 - Configure Device Dashboards From SignalK

Status: implemented for the SignalK side and firmware MVP; validation and
editor depth remain.

Definition of done:

- Operators can manage registered devices from SignalK.
- Operators configure dashboards through structured forms instead of raw JSON.
- Presets can be reused across devices with similar roles, display sizes, or
  capabilities.
- Presets can be imported/exported as JSON/YAML for review and version control.
- Saving a dashboard can queue a `config.reload` command.
- Devices can fetch, validate, apply, and report the active generated config
  hash.

Remaining:

- Complete a fully transactional rollback story for generated dashboard config
  apply failures. The current firmware keeps last-good managed render plans for
  parse/allocation failures, but not every config side effect can be rolled
  back atomically.
- Validate multi-screen and multi-widget generated configs on real firmware,
  beyond the current managed-screen MVP path.
- Add richer editor controls for adding/removing widgets, selecting SignalK
  paths, reordering tiles, and previewing layouts per device size.
- Extend end-to-end tests against a running SignalK server and mock/real
  firmware devices for more failure cases and larger generated configs.

### M2 - Device Management Client

Status: firmware MVP implemented; hardening and deployment validation remain.

Definition of done:

- Device discovers the manager from stored config, mDNS, SignalK well-known
  metadata, or manual serial/BLE setup.
- Device registers stable identity, board/display/touch capabilities, firmware
  metadata, and supported features.
- Device sends heartbeats with network, UI, touch, memory, SignalK, NMEA,
  firmware, OTA, config, and recent-error status.
- Device polls commands and acknowledges success, failure, expiry, or
  cancellation.
- Device applies hostname/domain config from SignalK and advertises mDNS
  services consistently.

Remaining:

- Harden manager discovery across Docker, boat LAN, setup AP, and mDNS-hostile
  networks.
- Expand status reconciliation and failure reporting for auth rotation,
  hostname conflicts, command expiry, and stale manager records.
- Finish hostname/domain edge cases: domain application, mDNS restart behavior,
  conflict reporting, and OTA address derivation on real deployments.

### M3 - Provisioning, Authentication, And Local Access Security

Status: SignalK plugin auth model exists and device tokens are hash-stored;
device-local hardening remains.

Definition of done:

- SignalK operator routes use normal SignalK authentication.
- Devices use per-device tokens for manager API calls.
- Token creation, rotation, revocation, and audit state are available.
- Devices can be claimed with an expiring provisioning token or signed claim.
- Device web/BLE write access is clearly locked down after provisioning.
- `/api/security` reports the actual local web/BLE access model.

Remaining:

- Add claim/provisioning flow to firmware beyond manual manager-register/token
  setup.
- Add local device web write protection for provisioned devices.
- Add BLE pairing, provisioning PIN, or signed manager claim before allowing
  persistent BLE configuration writes.
- Decide the supported trusted-LAN/setup-AP behavior for unclaimed devices.

### M4 - Firmware Catalog And OTA Fleet Management

Status: plugin-side artifact/job model and firmware pull-OTA MVP exist;
hardware failure-path validation and recovery policy remain.

Definition of done:

- Firmware artifacts can be tracked by vendor, product, board, version, build
  metadata, checksum, channel, compatibility, and storage location.
- Firmware jobs create `firmware.update` commands with artifact metadata and
  download URLs.
- Device downloads, verifies, installs, reboots, confirms boot, and reports
  progress.
- Plugin tracks job state through queued, delivered, downloading, applying,
  rebooting, confirmed, failed, and expired states.
- Failed updates have an explicit rollback or recovery story.

Remaining:

- Add hardware validation for successful update, failed checksum, interrupted
  download, failed boot confirmation, and rollback/retry behavior.
- Implement enforcement knobs for release channels once operator channel
  preferences are configurable.

### M5 - Discovery And Service Discovery Polish

Status: firmware and plugin discovery mechanisms exist; network validation and
operator polish remain.

Definition of done:

- The path from discovered device to claimed device to assigned preset to sent
  config is a single operator workflow.
- Discovery records expose address, port, services, display geometry,
  firmware, capabilities, stale state, and registration state.
- SignalK advertises management metadata in a way firmware can consume.
- Device advertises local web, OTA, and ESP display services consistently.

Remaining:

- Improve UI affordances for discovered-but-unclaimed devices.
- Continue stale/duplicate/conflict handling polish for device discovery
  records.
- Validate service discovery on real boat LANs, development Docker networks,
  and setup AP mode.

### M6 - Testing And CI Hardening

Status: plugin unit tests and native firmware tests pass locally; integration
coverage is still growing.

Definition of done:

- Plugin tests cover registry, profiles, generated configs, commands, firmware
  catalog/jobs, import/export, invalid input, and size limits.
- Native firmware tests cover parsers, generated config validation, manager
  config logic, and edge cases.
- System tests can run against a real SignalK plugin and a mock firmware
  device.
- Hardware-in-the-loop tests cover real display rendering, touch, web config,
  BLE config, manager commands, and OTA.
- Screenshots used in docs are generated by repeatable test sessions.

Remaining:

- Add e2e tests for SignalK UI flows and generated config convergence.
- Add mock firmware capabilities for different display sizes, resolutions, and
  widget/font support.
- Add real-device tests for `/api/dashboard/config.json`,
  `/api/dashboard/config.yaml`, and `/api/security`.
- Add failure-path tests for auth rejection, config validation errors, command
  expiry, and OTA failures.

### M7 - Public Beta Release

Status: not ready for broad public use.

Definition of done:

- README and docs clearly explain what works, what is experimental, and what is
  unsafe for production navigation.
- Screenshots show night mode, day mode, SignalK dashboard configuration, and
  device/preset workflows.
- Installation path covers firmware flashing, SignalK lab setup, manager UI
  access, and first managed dashboard push.
- Security caveats are prominent.
- Test status and supported hardware are current.
- Release binaries and tagged artifacts are reproducible.

Remaining:

- Clean committed versus runtime SignalK config data.
- Add a concise public-beta announcement and forum post draft.
- Confirm license/commercial-use language is visible.
- Verify fresh-clone setup on a clean machine.
- Run a final screenshot session before publishing.

## Feature Tracks

### SignalK Manager Plugin

Current surface:

- App-dock tile and plugin config link.
- Device registry, discovery, status, generated config, command queue,
  profiles/presets, groups, dashboard summary, firmware catalog/jobs, and
  automation endpoint.
- Structured UI for devices, per-device dashboard config, discovery, presets,
  preset apply, and firmware jobs.
- JSON/YAML dashboard import/export.

Next work:

- Richer dashboard editor and layout preview.
- Claim/provisioning workflow.
- Real SignalK integration tests with mock firmware.
- UI polish for discovery, stale devices, errors, and job progress.
- Stronger plugin data migration/versioning story.

### Embedded Firmware

Current surface:

- Core MFD screens and local UI.
- SignalK WebSocket data ingest.
- BLE diagnostics/config.
- Device-local web endpoints for layout/dashboard config.
- ArduinoOTA and local diagnostics.
- Host-portable manager config tests.

Next work:

- Harden manager discovery, registration recovery, auth rotation, heartbeat,
  commands, and config fetch failure modes.
- Complete generated dashboard config rollback semantics.
- Device-local dashboard web configurator polish.
- Device YAML policy: keep JSON-compatible YAML on device unless a small,
  bounded YAML parser becomes necessary.
- Pull OTA hardware validation, interrupted-download handling, boot confirmation
  failure handling, and rollback/retry policy.

### Security

Current model:

- SignalK protects operator/plugin routes.
- Device pull config uses a device token header in the contract.
- Device-local web and BLE configuration are documented as trusted-LAN or
  local-proximity setup surfaces.

Next work:

- Provisioning PIN or signed manager claim.
- Per-device token lifecycle in firmware for rotation/revocation recovery.
- Lock local web/BLE writes after claim unless explicitly re-enabled.
- Avoid storing or exporting WiFi passwords and SignalK bearer tokens in
  dashboard presets.

### Firmware Storage And Vendor Tracking

Current model:

- Plugin-side firmware artifacts and jobs are represented in the manager API.

Next work:

- Track provenance and review state before a firmware artifact can be offered
  as stable.
- Add garbage-collection policy for old local artifacts.

### Longer-Term Product Work

Potential later tracks:

- OpenHASP-style runtime widget addressing and dashboard automation.
- Companion app support over BLE and/or local web.
- More chart/history widgets and configurable data sources.
- NMEA 2000 direct support where hardware permits.
- Multiple board/display families with first-class geometry classes. The
  firmware now has compile-time Waveshare Tier 1 RGB profiles for 4.0, 4.3,
  4.3B, 5.0, 7.0, and 7B panels, plus geometry/status metadata for shape,
  density, usable area, touch, display bus, and layout class. Remaining work is
  hardware validation and separate display/touch drivers for non-RGB, round,
  portrait, SPI, QSPI, and AMOLED boards.
- Optional real DNS provider integration after mDNS/hostname management is
  stable.

## Known Gaps

- Firmware-side manager client has an MVP implementation, but is not yet
  validated end to end across real boat LANs, setup AP, SignalK auth modes,
  token rotation, and stale/conflicting registry records.
- Firmware-side OTA update application has a pull/checksum/progress/confirm
  implementation, but is not yet validated across real successful updates,
  interrupted downloads, failed boot confirmation, and rollback/retry cases.
- Device `.yaml` import/export is currently JSON-compatible YAML, not a full
  YAML parser.
- Device-local web and BLE write security need hardening before public or
  untrusted-network deployment.
- Dashboard editor is useful for structured settings but not yet a full visual
  layout builder.
- Hardware-in-the-loop coverage is still limited compared with host/plugin
  tests.
- Waveshare profiles beyond the 4 inch RGB path are compile/profile support
  until boot, backlight, touch coordinates, rotation, CAN/RS485, SignalK, OTA,
  auth, logs, and dashboard rendering are checked on physical boards.

## Related Specs

- [Implementation Plan](implementation-plan.md)
- [ESP Display Management Firmware Spec Plan](specs/17-espdisp-management-firmware-plan.md)
- [SignalK ESP Display Manager Plugin Spec Plan](specs/18-signalk-espdisp-manager-plugin-plan.md)
- [Device Display Widget Management](specs/19-device-display-widget-management.md)
- [Dashboard Import/Export And Access Security](specs/20-dashboard-config-import-export-security.md)
- [SignalK ESP Display Manager](signalk-espdisp-manager.md)
