# SignalK ESP Display Manager

The `espdisp-manager` plugin is the SignalK-side control plane for configuring
ESP display dashboards. It manages device registration, dashboard presets,
per-device configuration, display/layout variants, widget settings, command
delivery, firmware metadata, and OTA jobs.

Project-wide status, milestones, and remaining firmware/plugin/security/OTA
work are tracked in the [roadmap](roadmap.md).

At a high level this plugin lets a SignalK server act as the fleet manager for
small ESP displays. Operators register panels, group similar panels around
shared dashboard presets, tune the widgets/theme/font sizes from SignalK, and
then queue commands that cause devices to pull the latest generated dashboard
configuration. Firmware update metadata and OTA jobs use the same
registry/command/status model.

## Install the SignalK plugin

For a normal SignalK server, install the packed plugin tarball attached to the
matching GitHub release — see
[signalk/README.md → Install ESP Display Manager From This Repo](../signalk/README.md#install-esp-display-manager-from-this-repo).

To install the in-repo source directly (lab / development), the plugin lives at
`signalk/plugins/signalk-espdisp-manager/`. The repo-owned lab stack
(`make demo-up`) already mounts and enables it; install it into any other
SignalK server like this:

1. Copy or symlink the plugin into the SignalK plugin directory:

   ```sh
   cp -r signalk/plugins/signalk-espdisp-manager ~/.signalk/node_modules/
   # or, from a packed tarball built locally:
   #   npm pack signalk/plugins/signalk-espdisp-manager
   #   (cd ~/.signalk && npm install /path/to/signalk-espdisp-manager-<version>.tgz)
   ```

2. Restart the SignalK server.

3. In the SignalK admin UI (`http://<server>:3000`), open
   **Server → Plugin Config → ESP Display Manager**, enable it, and save. The
   plugin UI is then served at `/plugins/espdisp-manager/ui` (and the App Dock
   tile `ESP Displays`).

> **First-run admin user.** A fresh SignalK server has no users; the bundled
> `signalk/signalk-server` Docker image used by `make demo-up` rejects
> anonymous writes by default. The demo scripts assume the first admin user is
> created as **username `admin` / password `admin`** — log in with those
> credentials before enabling the plugin or issuing manager commands. On a real
> boat server, use your own SignalK admin credentials.

The lab fixture also uses the device-level token `espdisp-dev` in
`dev-shared-token` mode (see [Auth](#auth)); production-style deployments should
issue per-device provisioning tokens instead.

Once the plugin is enabled, flash and provision a display (or the
[remote knob](remote-knob.md)) so it registers with the manager.

## Concepts

```text
Device
  A physical ESP display. It reports identity, firmware, display geometry,
  touch support, widget support, font support, current UI state, and health.

Preset / Profile
  A reusable dashboard configuration bundle. The UI calls these presets; the
  API and store call them profiles. Presets can match a device by board,
  display size, role, location, or capability flags.

Device Override
  Per-device settings layered on top of the assigned preset. Overrides are used
  for local differences such as brightness, font sizes, touch mode, or NMEA
  host without cloning the entire preset.

Generated Config
  The per-device dashboard config emitted by the plugin after merging profile
  defaults, per-device overrides, display variant selection, widget filtering,
  and font size resolution.

Command
  A queued action for one device. Devices poll, execute, then acknowledge.

Group
  A dynamic set of devices. Current groups are derived from role, location,
  and the built-in all group.

Firmware Artifact
  Vendor-aware firmware metadata plus optional binary storage information.

Firmware Job
  A firmware update request that creates a firmware.update command and tracks
  progress through confirmation.
```

## Operator Workflow

The built-in web UI is server-rendered and available inside SignalK:

```text
/plugins/espdisp-manager/ui
/signalk-espdisp-manager/
```

It is also exposed as the `ESP Displays` App Dock tile in the repo-owned
SignalK test configuration.

Typical workflow:

```text
1. Start the SignalK lab stack with make demo-up.
2. Register or discover one or more ESP displays.
3. Claim discovered panels from Discovery, optionally selecting the first
   dashboard preset.
4. Open Devices and inspect health, display geometry, config drift, and
   pending commands.
5. Open a device config page to assign a dashboard preset and edit structured
   settings.
6. Save per-device overrides or save the same settings as a reusable preset.
7. Use "Save and send" or preset "Apply" to queue config.reload.
8. Device polls commands, sees config.reload, fetches /devices/:id/config,
   applies the generated dashboard config, and reports the applied hash in
   status.
```

The UI does not expose a raw JSON editor for generated config. Generated config
is still available to firmware through the authenticated JSON API, but operator
pages render structured sections for dashboard theme, default screen, network,
SignalK/NMEA, OTA, autopilot widgets, debug/touch mode, widget font sizes, and
screens.

Dashboard presets can be exported from SignalK as JSON or YAML, committed for
review, and imported back into the manager. Devices expose matching local web
endpoints for dashboard config import/export when direct bench configuration is
needed.

## Visual Layout Builder

Status: planned. The implemented dashboard editor is a structured form/table
editor, not a visual drag/drop builder.

Current implemented editor:

- assigns a preset/profile to one device
- edits device overrides for theme, brightness, default screen, NMEA, autopilot,
  debug/touch options, and widget font sizes
- adds/removes widget definitions with id, title, type, SignalK path, unit,
  precision, and value font size
- edits screen ids and tile placement through row/column fields
- saves the current device settings or stores them as a reusable preset
- exports/imports presets through `espdisp.dashboard.v1` JSON/YAML

The visual layout builder should sit on top of the same dashboard schema. It
should not introduce a second config format. Its job is to make common layout
changes visible and safe while still producing the existing `widgets.items` and
`layout.screens[].tiles[]` structures consumed by firmware.

Planned operator experience:

1. Select a target device or display class.
2. Show a canvas whose aspect ratio, resolution label, rotation, and supported
   widget list come from registered device geometry/capabilities.
3. Add widgets from a palette of supported widget types.
4. Pick SignalK paths from known/self vessel paths, recently seen paths, or
   typed fallback input.
5. Drag widgets onto a grid, resize/reorder them, and switch between screens.
6. Preview day/night theme, font sizing, and unsupported-widget filtering.
7. Save as device override, save as preset, or save and send `config.reload`.

Required controls:

- device/display selector
- screen tabs with add/duplicate/delete
- widget palette grouped by numeric, text, gauge, bar, compass, wind, trend,
  button, and autopilot widgets
- property inspector for the selected widget/tile
- SignalK path picker with unit and precision hints
- grid snap controls and density presets
- validation panel for errors and warnings
- JSON/YAML export for review/debug

Validation rules:

- widget ids must be unique and firmware-safe
- every tile must reference an existing widget
- tile positions must fit the selected screen grid
- overlapping tiles should be rejected unless the layout type explicitly
  supports stacking
- unsupported widget types must be hidden or flagged before save
- required SignalK paths should be present or marked as unresolved
- font sizes should resolve to device-supported values before send

Implementation outline:

1. Extract a small dashboard model module from the current form parsing logic.
   It should normalize widget ids, screens, tiles, variants, font sizes, and
   validation messages without depending on HTML rendering.
2. Add API endpoints for editor metadata: registered display classes, supported
   widget types, discovered SignalK paths, templates, and validation.
3. Build the visual editor as progressive enhancement for the existing server
   UI path. A non-JavaScript form fallback should remain available for lab and
   recovery use.
4. Store builder output as the existing preset/device override shape. Do not
   store canvas-only state unless it can be ignored by firmware and older
   plugin versions.
5. Add Playwright coverage for create/edit/apply flows and snapshot the
   generated JSON to catch accidental schema drift.
6. Validate on at least one square 480x480 profile and one wide 800x480 or
   1024x600 profile before marking it implemented.

Out of scope for the first visual builder:

- real-time rendering of firmware LVGL internals
- arbitrary SVG/CSS authoring
- multi-user collaborative editing
- custom JavaScript widgets downloaded to devices

## Documentation Critique And Suggested Edits

The current documentation is useful but too optimistic in places. It describes
presets, generated configs, and device management well, but it can make the UI
sound closer to a visual dashboard builder than it actually is.

Specific problems:

- "dashboard config" and "dashboard editor" are used broadly without always
  saying whether the feature is implemented as structured forms or still
  planned as a visual builder.
- Screenshots show the current operator UI, but there is no feature/status
  table next to them, so readers cannot quickly separate implemented controls
  from roadmap intent.
- The README buries the visual-builder gap under general manager behavior. A
  new reader may assume drag/drop layout editing exists because widgets,
  screens, presets, and layout variants are all implemented concepts.
- The roadmap says the editor is not yet a full visual layout builder, but the
  main manager guide did not previously define what "full visual layout
  builder" means.
- Import/export, generated config, device-local config endpoints, and presets
  are documented in several places. The duplication is helpful for discovery,
  but it risks drifting into inconsistent status claims.

Suggested edits:

- Add a short "Implemented / Planned" table near every major UI section.
- Keep `signalk/README.md` focused on how to run and verify the lab plugin;
  link to this file for product scope and roadmap details.
- Move long API endpoint lists into one canonical API reference section and
  link to it from README/docs instead of repeating it.
- Add annotated screenshots or captions that state "structured editor" rather
  than implying visual layout editing.
- Add a builder milestone checklist to the roadmap with acceptance criteria:
  canvas preview, widget palette, path picker, validation, import/export
  compatibility, and Playwright coverage.
- Add a small schema example that shows exactly how a visual canvas maps to
  `widgets.items` and `layout.screens[].tiles[]`.

## Device States

The Devices page shows one health state per registered device. States are
derived in this order: offline, config-error, pending, network-conflict, then
ok. If more than one condition is true, the earlier state in that order wins.

| State | Meaning | How to solve it | How to avoid it |
|---|---|---|---|
| `ok` | The device is online, has no failed config report, has no pending/delivered commands, and its desired hostname is unique. | No action needed. If the device still behaves oddly, open the device detail page and inspect live status, logs, reported config hash, and recent commands. | Keep profiles stable, avoid unnecessary hostname changes, and let devices complete command acknowledgements before issuing more changes. |
| `offline` | The manager has not seen a heartbeat recently. The online window is `max(heartbeatMs * 3, 15000)` milliseconds; the plugin clamps heartbeat to at least `30000` ms because firmware does the same. This state hides lower-priority states until the device reports again. | Power the device, confirm WiFi, confirm it can reach SignalK, check the device's manager endpoint/token, then restart or re-register if needed. Use Discovery or an IP scan if the address changed. | Use stable WiFi, keep the SignalK host reachable from the display network, avoid AP isolation, and keep manager discovery/registration settings current. |
| `config-error` | The device heartbeat reported `config.applied === false`. The device rejected or only partially applied its last generated config. | Open the device detail/config page, compare desired and reported config hashes, inspect recent errors/logs, then fix unsupported widgets, invalid paths, bad layout references, invalid brightness/theme/touch settings, or malformed network/source config. Queue `config.reload` after fixing. | Use the structured config UI, keep widget IDs and screen tile references consistent, avoid unsupported widget types for that device, and test presets on a mock or lab device before applying broadly. |
| `pending` | The device has at least one non-expired command in `pending` or `delivered` state. This includes commands such as `config.reload`, screen/theme/brightness changes, and firmware jobs. | Wait for the device to poll and acknowledge. If it stays pending, check that the device is online, command polling is running, auth tokens are valid, and the command has not expired. Cancel stale commands if appropriate, then retry. | Do not queue repeated reloads faster than the device's command poll interval, keep command payloads valid, and confirm the device is heartbeating before sending commands. |
| `network-conflict` | The manager calculated the same desired hostname for another registered device. This is usually caused by duplicate device IDs, identical manual hostnames, or a role/location naming policy that maps two devices to the same name. | Open each conflicting device, change one device's ID, role/location, manual hostname, or the manager network naming policy so each desired hostname is unique. Then send config reload or let the next heartbeat/config cycle update the device. Remove stale duplicate registry records if a device was replaced. | Use unique device IDs, avoid assigning the same manual hostname to multiple displays, choose a naming policy that includes enough unique information, and clean up old registry entries after replacing hardware. |

Discovery rows have separate freshness/conflict indicators for unclaimed
records. Those are about observed network advertisements and duplicate
addresses. The registered device `network-conflict` state is specifically
about duplicate desired hostnames in the registry.

## Screenshots

Manager overview:

![ESP Display Manager overview](images/signalk-manager-overview.png)

Device dashboard configuration:

![ESP Display Manager device configuration](images/signalk-manager-device-config.png)

Day dashboard preset:

![ESP Display Manager day dashboard configuration](images/signalk-manager-device-config-day.png)

Presets:

![ESP Display Manager presets](images/signalk-manager-presets.png)

Preset apply:

![ESP Display Manager preset apply](images/signalk-manager-preset-apply.png)

Day preset apply:

![ESP Display Manager day preset apply](images/signalk-manager-preset-apply-day.png)

## Device Lifecycle

```text
1. Device discovers SignalK and the espdisp-manager plugin.
2. Device registers with identity, display geometry, and capabilities.
3. Plugin assigns an explicit or auto-matched profile.
4. Device fetches generated config.
5. Device applies config and reports status/heartbeat.
6. Plugin queues commands.
7. Device polls, executes, and acknowledges commands.
8. Firmware jobs use the same command/progress/confirm loop.
```

## Implemented Plugin Surface

Base path:

```text
/plugins/espdisp-manager
```

The package also declares `signalk-webapp` metadata so SignalK/App Dock can
discover it as an installed app. The webapp entrypoint redirects to the plugin
UI:

```text
/signalk-espdisp-manager/
```

The repo-owned SignalK lab config adds this app to `@signalk/app-dock` as the
`ESP Displays` tile.

Discovery and capabilities:

```text
GET  /.well-known/espdisp-management
GET  /capabilities
GET  /dashboard
GET  /ui
GET  /ui/devices
GET  /ui/devices/:id
GET  /ui/devices/:id/config
POST /ui/devices/:id/config
GET  /ui/discovery
GET  /ui/profiles
GET  /ui/profiles/:id
POST /ui/profiles/:id/apply
GET  /ui/firmware
POST /ui/firmware/catalog/refresh
POST /ui/devices/:id/firmware/update
GET  /discovery/devices
POST /discovery/devices
POST /discovery/devices/:id/claim
```

Discovery sources:

```text
Manager advertisement: Bonjour/mDNS _espdisp-mgmt._tcp.local
Bonjour/mDNS _espdisp._tcp.local
UDP espdisp.device.announce.v1 on port 34301
Authenticated HTTP POST /discovery/devices
```

The manager advertisement is optional because some Docker and boat-router
deployments do not forward multicast DNS. When enabled, it publishes protocol,
base path, auth mode, TLS flag, SignalK port, and NMEA TCP port so firmware
can use the `manager-discover` path before falling back to well-known HTTP or
manual setup.

Bonjour TXT records are treated as live discovery metadata. The firmware
refreshes them on boot, every minute, and after managed config or web auth
changes. The plugin stores those updates with `source=mdns`; UDP
announcements use `source=udp`. Both paths populate the same Discovery page
and claim flow. Discovery records also carry freshness, previous addresses,
and address-conflict metadata so operators can spot stale leases, duplicate
advertisements, and two device IDs reporting the same endpoint before claiming
the device.

Registry:

```text
GET    /devices
POST   /devices/register
GET    /devices/:id
PATCH  /devices/:id
POST   /devices/:id/status
GET    /devices/:id/config
GET    /devices/:id/auth/status
```

Device discovery is separate from registration. A device, scanner, or fixture
can announce an address and capabilities before the panel has been claimed:

```json
{
  "device": {
    "id": "espdisp-aabbccddeeff",
    "name": "Helm Display",
    "address": "192.168.50.42",
    "port": 80,
    "services": [
      { "type": "_espdisp._tcp", "port": 80 },
      { "type": "_arduino._tcp", "port": 3232 }
    ],
    "display": { "width": 480, "height": 480, "shape": "square" },
    "firmware": { "version": "0.2.0-alpha" }
  }
}
```

Firmware also broadcasts the same information as UDP
`espdisp.device.announce.v1` on port `34301`. The plugin listens on that port
and writes the same discovery record as the authenticated HTTP announcement
endpoint.

`GET /discovery/devices` returns announced devices with `registered` and
`stale` flags. Operators can claim an announced device into the registry with:

```text
POST /discovery/devices/:id/claim
```

The claim body may include `profileId`, `role`, `location`, `sendReload`, and
`issueToken`. Claiming creates or updates the registry record, marks it
claimed, assigns the selected preset, and can queue an initial `config.reload`
command. Firmware registration still goes through `POST /devices/register`;
when the same device later registers, the claimed profile and registry state
are preserved.

Claimed registry records carry these explicit sub-objects:

```json
{
  "claim": {
    "claimedAt": "2026-05-29T00:00:00.000Z",
    "updatedAt": "2026-05-29T00:00:00.000Z",
    "source": "discovery",
    "profileId": "default"
  },
  "discovery": {
    "source": "udp",
    "address": "192.168.50.42",
    "port": 80,
    "lastSeen": "2026-05-29T00:00:00.000Z",
    "services": [{ "type": "_espdisp._tcp", "port": 80 }],
    "authRequired": true
  },
  "auth": {
    "web": {
      "required": true,
      "mode": "basic",
      "username": "espdisp",
      "passwordSet": true
    },
    "manager": { "mode": "dev-shared-token" }
  }
}
```

Profiles and groups:

```text
GET  /profiles
POST /profiles
GET  /profiles/:id/dashboard.json
GET  /profiles/:id/dashboard.yaml
POST /profiles/import-dashboard
POST /profiles/:id/apply
POST /devices/:id/profile
GET  /groups
POST /groups/:groupId/command
```

`POST /profiles/:id/apply` accepts `deviceIds`, `clearOverrides`, and
`sendReload`. It assigns the preset to several devices through the same path as
the UI and optionally queues `config.reload` for each selected device.

Commands:

```text
POST /devices/:id/command
GET  /devices/:id/commands
GET  /devices/:id/commands/:commandId
POST /devices/:id/commands/:commandId/ack
POST /devices/:id/commands/:commandId/cancel
POST /automation/event
```

Provisioning and tokens:

```text
GET  /provisioning/tokens
POST /provisioning/tokens
POST /devices/:id/tokens/rotate
POST /devices/:id/tokens/revoke
```

Firmware:

```text
GET  /firmware/catalog
POST /firmware/catalog/refresh
POST /firmware/artifacts
GET  /firmware/artifacts/:artifactId
GET  /firmware/download/:jobId
GET  /devices/:id/firmware/jobs
POST /devices/:id/firmware/jobs
GET  /devices/:id/firmware/jobs/:jobId
POST /devices/:id/firmware/jobs/:jobId/progress
POST /devices/:id/firmware/confirm
```

## Auth

SignalK protects plugin HTTP routes with normal SignalK auth:

```text
Authorization: Bearer <signalk-token>
```

Device-level auth is carried separately:

```text
X-EspDisp-Authorization: Bearer <device-token>
```

Registry and provisioning token storage:

- Per-device tokens are stored as `sha256:<hex>` hashes in the plugin registry.
  The plaintext token is returned only when a device is first registered,
  claimed with token issue enabled, or explicitly rotated.
- One-time provisioning tokens are also stored as hashes. The plaintext token is
  returned only from the creation call.
- Legacy registry or provisioning records with plaintext tokens are migrated to
  hashes the next time the token is successfully used.
- The `dev-shared-token` mode is for lab use only; production-style deployments
  should use provisioning tokens plus per-device bearer tokens.

The local test fixture uses:

```text
username: admin
password: admin
device token: espdisp-dev
```

## Display And Widget Configuration

Devices register display geometry:

```json
{
  "display": {
    "width": 480,
    "height": 480,
    "rotation": 0,
    "colorDepth": 16,
    "shape": "square"
  }
}
```

Profiles can match by geometry:

```json
{
  "id": "wide-helm",
  "priority": 50,
  "match": {
    "display": {
      "width": 800,
      "height": 480
    }
  }
}
```

Profiles can include layout and widget variants. The plugin sends only the
selected variant and filters unsupported widgets before the device sees the
config.

Font sizes are resolved against the device-reported supported font sizes. If a
profile requests `50` and the device supports `[12, 24, 48]`, the generated
config uses `48`.

## Presets And Device Configuration

Presets are the main operator abstraction for managing several similar devices.
A preset can hold:

- screen/default behavior such as default screen, theme, brightness, and demo
  mode
- NMEA 0183 over WiFi settings
- autopilot UI capability flags
- widget defaults including font sizes
- display layout and widget variants
- debug/touch options

The device config page supports four actions:

```text
Save device
  Stores the selected preset plus per-device overrides. The device will pick
  this up the next time it fetches config.

Save and send to device
  Stores the settings and queues config.reload with the generated config hash.

Save as preset
  Stores the current structured settings as a reusable preset/profile.

Save preset and send
  Creates or updates a preset, assigns it to the device, and queues reload.
```

The preset detail page lists all registered devices with checkboxes. Applying a
preset can optionally clear existing device overrides and queue `config.reload`
for every selected device. This is the path for making several panels of the
same size or role converge on the same configuration.

Generated config is deterministic apart from `generatedAt`. The config hash is
computed without `generatedAt`, so devices can compare hashes to detect drift.

## Dashboard Import/Export

SignalK preset export:

```text
GET /plugins/espdisp-manager/profiles/:id/dashboard.json
GET /plugins/espdisp-manager/profiles/:id/dashboard.yaml
```

SignalK preset import:

```text
POST /plugins/espdisp-manager/profiles/import-dashboard
```

The imported document kind is `espdisp.dashboard.v1`. The `dashboard` object is
stored as the preset/profile config:

```yaml
kind: espdisp.dashboard.v1
preset:
  id: helm-day
  name: Helm Day
dashboard:
  settings:
    defaultScreen: dashboard
    theme: day
    brightness: 0.85
  widgets:
    defaults:
      valueFontSize: 48
```

Device-local import/export aliases:

```text
GET /api/dashboard/config.json
PUT /api/dashboard/config.json
GET /api/dashboard/config.yaml
PUT /api/dashboard/config.yaml
```

JSON is canonical. SignalK emits block-style YAML and imports that constrained
profile. The device `.yaml` endpoint uses JSON-compatible YAML syntax to avoid a
large YAML parser in firmware.

Device access security is intentionally explicit:

- SignalK operator routes use SignalK bearer auth.
- Device config pulls use `X-EspDisp-Authorization`.
- Device web endpoints currently have no local HTTP auth and are intended for
  trusted LAN or temporary setup AP use.
- Device BLE configuration currently has no pairing/application auth and is
  intended for local physical proximity setup.
- `/api/security` reports the active device web/BLE access model.

Threat model:

| Environment | Assumption | Required practice |
|---|---|---|
| Lab LAN / `yey-net` | Developers control the AP, SignalK server, and devices. Debug builds and shared development tokens may be used. | Keep lab credentials out of boat presets, rotate tokens after shared sessions, and do not expose lab SignalK to public networks. |
| Trusted boat LAN | The boat router/firewall protects SignalK, ESP displays, OTA, and local web endpoints from marina/public clients. | Use SignalK authentication, per-device manager tokens, reserved DHCP or stable hostnames, and keep USB recovery available before OTA. |
| Setup AP / first provisioning | The device may temporarily expose local setup surfaces before it joins the boat LAN. | Use only during installation, avoid entering long-lived secrets on untrusted clients, and move the device to the trusted LAN before managed operation. |
| Public or shared marina WiFi | Other clients may scan, replay, or attempt writes to local HTTP/BLE surfaces. | Do not place displays directly on this network. Use an onboard AP/VLAN or firewall rules that block unsolicited access. |

## Command Flow

Commands are durable until delivered, acknowledged, cancelled, failed, or
expired.

```text
Operator/UI/API
  POST command or save config with "send"
    -> plugin stores command as pending
Device
  GET /devices/:id/commands
    -> plugin marks returned commands as delivered
Device
  execute command
Device
  POST /devices/:id/commands/:commandId/ack
    -> plugin records acknowledged/failed result
```

For config changes the command type is currently:

```json
{
  "type": "config.reload",
  "payload": {
    "version": 1,
    "hash": "sha256:...",
    "url": "/plugins/espdisp-manager/devices/<device-id>/config"
  }
}
```

Firmware jobs follow the same command pattern using `firmware.update`, plus
job progress and boot confirmation endpoints.

### GitHub Release Firmware Source

For onboard software upgrades, the manager can populate its firmware catalog
from this repository's GitHub releases. The release workflow publishes one
target-prefixed merged image per supported board, plus `SHA256SUMS`:

```text
esp32-4848s040-merged_firmware.bin
waveshare-touch-lcd-4-merged_firmware.bin
waveshare-touch-lcd-4_3-merged_firmware.bin
waveshare-touch-lcd-4_3b-merged_firmware.bin
waveshare-touch-lcd-5_800x480-merged_firmware.bin
waveshare-touch-lcd-5_1024x600-merged_firmware.bin
waveshare-touch-lcd-7_800x480-merged_firmware.bin
waveshare-touch-lcd-7b_1024x600-merged_firmware.bin
SHA256SUMS
```

`POST /plugins/espdisp-manager/firmware/catalog/refresh` reads the latest
GitHub release, imports assets whose names match supported board ids, records
their SHA-256 checksums, and stores the GitHub download URL. Firmware update
jobs created from those artifacts send `firmware.update` commands with the
GitHub asset URL, target version, size, and SHA-256.

The SignalK operator UI exposes the same flow on `GET /ui/firmware`:

- `Device upgrade status` compares every registered device with compatible
  catalog artifacts.
- `Available versions` lists artifact versions compatible with the device
  board/chip and marks the currently installed version.
- `Queue update` creates a firmware job and enqueues a `firmware.update`
  command for that device.
- `Refresh catalog from GitHub` imports the newest compatible release assets
  before queuing updates.

The command is pull-based. If an artifact has a GitHub release URL, firmware
downloads from GitHub. If the artifact is stored by the plugin instead, the
command URL points at SignalK:

```text
/plugins/espdisp-manager/firmware/download/<job-id>
```

That SignalK endpoint streams the stored firmware binary when the artifact has
`file.path`; GitHub-backed artifacts are downloaded directly from the GitHub
asset URL in the command payload.

In both cases the command includes `jobId`, `artifactId`, `version`, `url`,
`sha256`, `size`, `mode: pull`, `reboot`, and `confirmAfterBoot`. Firmware
reports progress with `POST /devices/:id/firmware/jobs/:jobId/progress` and
confirms the booted version with `POST /devices/:id/firmware/confirm`.

Firmware storage and channel policy:

| Source | Intended use | Requirements |
|---|---|---|
| GitHub release asset | Stable or prerelease onboard upgrades imported by `firmware/catalog/refresh`. | Asset name must match a supported release target, `SHA256SUMS` must include the asset, and channel is derived from the GitHub release prerelease flag. |
| Plugin-local artifact | Lab or vendor artifact uploaded/registered through the manager API. | Metadata must include vendor, product, board compatibility, version/channel, size, and SHA-256 before it is offered to a device. |
| External vendor URL | Future vendor-hosted firmware. | Must include provenance, compatibility, size, SHA-256, and explicit review/trust metadata before it can be marked stable. |

Channels:

- `lab` and `dev` artifacts are for local testing and should not be offered as
  stable boat updates.
- `prerelease` / `beta` artifacts may be offered only when the operator opts
  into prerelease updates.
- `stable` artifacts require checksum metadata, compatible board targeting,
  and either a trusted GitHub release asset or reviewed vendor metadata.

## Dashboard API

`GET /dashboard` returns an operator summary:

```json
{
  "counts": {
    "devices": 2,
    "online": 1,
    "offline": 1,
    "configDrift": 1,
    "pendingCommands": 3
  },
  "devices": [
    {
      "id": "espdisp-aabbccddeeff",
      "health": "ok",
      "profile": "default",
      "display": {
        "width": 480,
        "height": 480
      },
      "desiredConfig": {
        "layoutVariant": "square-480",
        "widgetVariant": "square-480"
      }
    }
  ]
}
```

`GET /ui` renders a lightweight HTML operator console backed by the same JSON
APIs. It has server-rendered pages for overview, registered devices, device
detail, structured device config, discovery, presets, preset detail/apply, and
firmware jobs. A richer SignalK webapp can replace it later without changing
the API.

## Test Commands

Run plugin tests:

```sh
npm test --prefix signalk/plugins/signalk-espdisp-manager
```

The plugin suite includes a mock-firmware end-to-end flow that announces a
device, claims it from discovery, queues `config.reload`, applies a generated
dashboard config, swaps presets, and verifies the reported config hash
converges.

Run the manager control-plane load benchmark:

```sh
npm run load-test --prefix signalk/plugins/signalk-espdisp-manager -- \
  --url http://localhost:3000 \
  --devices 20 \
  --duration-sec 180 \
  --username admin \
  --password admin
```

The benchmark registers synthetic devices, sends status heartbeats, polls
commands, optionally fetches config, and reports request latency percentiles.
See [SignalK Manager Load and Async Design](architecture/signalk-manager-load-and-async.md)
for capacity assumptions and the proposed async persistence architecture.

Run firmware contract tests in skip mode:

```sh
pytest tests/system/unattended/test_espdisp_manager_contract.py -q
```

Run firmware contract tests against a real device:

```sh
YEYBOATS_MANAGER_CONTRACT=1 \
YEYBOATS_HOST=<device-ip> \
SIGNALK_URL=http://localhost:3000 \
pytest tests/system/unattended/test_espdisp_manager_contract.py
```
