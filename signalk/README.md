# SignalK Test Server

This directory contains the repo-owned SignalK test server configuration.

It is used for local firmware testing with:

- SignalK HTTP/WebSocket on `localhost:3000`
- NMEA 0183 TCP output on `localhost:10110`
- ESP display SignalK discovery responder on UDP `34300`
- ESP display device announcement listener on UDP `34301`
- official NMEA 0183 conversion plugin
- official autopilot plugin in emulator mode
- synthetic boat data from `tools/fake_boat.py`
- repo-owned `espdisp-manager` plugin for display registry, dashboard presets,
  generated dashboard config, command delivery, and firmware job testing

Project-level manager concepts and screenshots are documented in
[`docs/signalk-espdisp-manager.md`](../docs/signalk-espdisp-manager.md).

Current manager UI screenshots are stored under `docs/images/`:

```text
signalk-manager-overview.png
signalk-manager-devices.png
signalk-manager-device-config.png
signalk-manager-device-config-day.png
signalk-manager-presets.png
signalk-manager-preset-apply.png
signalk-manager-preset-apply-day.png
```

## Start

```sh
make demo-up
```

or directly:

```sh
./signalk/scripts/run.sh
```

The repo test stack installs the local plugin from
`signalk/plugins/signalk-espdisp-manager` through
`signalk/config/package.json`:

```json
"signalk-espdisp-manager": "file:../plugins/signalk-espdisp-manager"
```

That path is the fastest way to test plugin changes from this checkout. The
run script starts SignalK with:

```text
-v signalk/config:/home/node/.signalk
-v signalk/plugins:/home/node/plugins
```

and runs `npm install` inside `/home/node/.signalk` before starting the server.

## Stop

```sh
make demo-down
```

or directly:

```sh
./signalk/scripts/stop.sh
```

## Install ESP Display Manager From This Repo

For a normal SignalK server, install the packed plugin tarball attached to the
matching GitHub release. Releases include firmware binaries and:

```text
signalk-espdisp-manager-<version>.tgz
SHA256SUMS
```

Download both files from the release page. On the SignalK host, optional
checksum verification from the download directory is:

```sh
sha256sum -c SHA256SUMS --ignore-missing
```

Install the release asset into the SignalK home directory:

```sh
cd ~/.signalk
npm install /path/to/signalk-espdisp-manager-<version>.tgz
```

Then restart SignalK and enable/configure `ESP Display Manager` in the SignalK
admin plugin UI. The plugin UI is served at:

```text
/plugins/espdisp-manager/ui
/signalk-espdisp-manager/
```

The release package is the recommended install path because it is built by CI
from the tagged source and published alongside the matching firmware artifacts.
Every push/PR also uploads a `signalk-espdisp-manager-<sha>` workflow artifact
for branch testing, but those artifacts are not release-pinned.

### OTA Password In CI And Release Builds

Firmware reads ArduinoOTA auth from `OTA_PASSWORD` in generated
`include/secrets.h`. The repository does not store this file. GitHub Actions
generates it from repository secrets:

```text
ESPDISP_OTA_PASSWORD
```

The regular CI firmware matrix uses this secret when available. The tagged
release workflow requires it and fails before building firmware artifacts if it
is missing, so release images are not accidentally built with unauthenticated
ArduinoOTA.

For local builds, set the same environment variable before `make build` or
`make ota`:

```sh
ESPDISP_OTA_PASSWORD='<ota-password>' make build
ESPDISP_OTA_PASSWORD='<ota-password>' make ota DEVICE_IP=<device-ip>
```

The OTA password is embedded in the firmware image. If release assets are
public or distributed outside the intended deployment group, rotate the
password.

## Firmware Upgrade Artifacts From GitHub

The ESP Display Manager firmware catalog can import firmware artifacts directly
from this repository's GitHub releases. Release assets are versioned by Git tag
and board target, for example:

```text
esp32-4848s040-merged_firmware.bin
waveshare-touch-lcd-7b_1024x600-merged_firmware.bin
SHA256SUMS
```

When the GitHub release source is enabled, the plugin reads the latest release,
matches `*-merged_firmware.bin` assets against supported board ids, verifies
that each asset is listed in `SHA256SUMS`, and adds catalog entries with:

- firmware version from the release tag
- board compatibility from the asset name
- SHA-256 from `SHA256SUMS`
- GitHub release download URL for device pull-OTA jobs

Refresh the catalog from the API:

```sh
curl -X POST http://<signalk-host>:3000/plugins/espdisp-manager/firmware/catalog/refresh
```

The device firmware update command then points at the GitHub asset URL and
includes the matching SHA-256 so firmware can verify the downloaded image.

### Build A Local Plugin Package

For development, build a local tarball from the repo checkout:


```sh
cd signalk/plugins/signalk-espdisp-manager
npm ci
npm test
npm pack
```

That creates:

```text
signalk-espdisp-manager-<version>.tgz
```

Install it into the SignalK home directory on the target server:

```sh
cd ~/.signalk
npm install /path/to/espdisp/signalk/plugins/signalk-espdisp-manager/signalk-espdisp-manager-<version>.tgz
```

## Verify SignalK

```sh
curl http://localhost:3000/signalk
```

ESP display firmware in default `sk-host auto` mode first tries mDNS
`_signalk-ws._tcp`. If that is not advertised by the LAN or Docker setup, it
broadcasts `espdisp.signalk.discover.v1` to UDP `34300`. The manager plugin
replies with the SignalK HTTP/WebSocket port; the device uses the reply source
address when no explicit advertised host is configured, then TCP-probes the
target before opening the SignalK WebSocket.

For manager discovery, the plugin can also advertise `_espdisp-mgmt._tcp.local`
with TXT metadata for protocol, base path, auth mode, TLS flag, SignalK port,
and NMEA TCP port. Firmware can query this with `manager-discover`. This works
on LANs or containers where multicast DNS UDP `5353` reaches the display.

ESP display firmware also broadcasts `espdisp.device.announce.v1` to UDP
`34301` after joining WiFi. The manager plugin listens on that port and adds
unclaimed displays to the Discovery page without needing a subnet scan.

The firmware also advertises Bonjour/mDNS service `_espdisp._tcp.local`.
The manager plugin passively listens for those advertisements and uses the
TXT fields (`device_id`, `board`, `firmware`, `version`, `display`, `auth`,
`cfg_ver`, `cfg_hash`, `seq`) to update the same Discovery page. Firmware
refreshes the Bonjour TXT records on boot, periodically, and after managed
configuration/auth changes.

## Discovery Troubleshooting

From the repo root, verify device visibility:

```sh
python3 -m tests.system.discovery --json --listen-udp --timeout 5
```

If the device does not appear:

- check the display is connected to the same WiFi as SignalK
- check firmware logs for `[mdns] advertise` and `[discovery] announce`
- confirm mDNS multicast UDP `5353` is allowed between the SignalK host and
  display network
- confirm UDP `34301` is exposed when SignalK runs in Docker
- use an explicit host as a fallback:

```sh
ESPDISP_HOST=<device-ip> pytest tests/system/unattended/test_boot_health.py
```

## Verify NMEA 0183 TCP

After the fake boat producer is running:

```sh
nc localhost 10110
```

Expected sentences include `GGA`, `RMC`, `HDT`, `MWV`, `VWR`, `VWT`, `DBT`,
and `MTW` when the required SignalK paths have data.

## Verify Autopilot Emulator

```sh
curl http://localhost:3000/signalk/v1/api/vessels/self/steering/autopilot/state/value
```

Expected default:

```text
"standby"
```

Authenticated state command example:

```sh
TOKEN=$(curl -s -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin"}' \
  http://localhost:3000/signalk/v1/auth/login | jq -r .token)

curl -s -X PUT \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"value":"auto"}' \
  http://localhost:3000/signalk/v1/api/vessels/self/steering/autopilot/state
```

Then query the state path again.

## Verify ESP Display Manager

SignalK protects plugin routes with its normal HTTP auth. Device management
auth is carried separately with `X-EspDisp-Authorization`.

```sh
TOKEN=$(curl -s -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin"}' \
  http://localhost:3000/signalk/v1/auth/login | jq -r .token)

curl -s \
  -H "Authorization: Bearer $TOKEN" \
  http://localhost:3000/plugins/espdisp-manager/.well-known/espdisp-management

curl -s -X POST \
  -H "Authorization: Bearer $TOKEN" \
  -H "X-EspDisp-Authorization: Bearer espdisp-dev" \
  -H 'Content-Type: application/json' \
  -d '{"device":{"id":"espdisp-aabbccddeeff","board":"esp32-4848s040"}}' \
  http://localhost:3000/plugins/espdisp-manager/devices/register
```

The built-in operator UI is available at:

```text
http://localhost:3000/plugins/espdisp-manager/ui
```

It is also exposed as an installed SignalK webapp and as an App Dock tile named
`ESP Displays`:

```text
http://localhost:3000/signalk-espdisp-manager/
http://localhost:3000/@signalk/app-dock/
```

UI pages:

```text
/plugins/espdisp-manager/ui
/plugins/espdisp-manager/ui/devices
/plugins/espdisp-manager/ui/devices/:id
/plugins/espdisp-manager/ui/devices/:id/config
/plugins/espdisp-manager/ui/discovery
/plugins/espdisp-manager/ui/profiles
/plugins/espdisp-manager/ui/profiles/:id
/plugins/espdisp-manager/ui/firmware
```

The operator UI is intentionally structured, not a raw JSON editor:

- the device page shows current status, firmware jobs, and recent commands
- the device config page edits the dashboard from SignalK: preset assignment,
  day/night theme, default screen, data sources, autopilot flags, widget font
  sizes, and debug/touch options
- `Save and send to device` saves the generated config inputs and queues a
  `config.reload` command for the device to poll
- `Save as preset` stores the same settings as a reusable profile/preset
- preset rows export dashboard configs as JSON or YAML
- the preset import form accepts `espdisp.dashboard.v1` JSON/YAML
- the preset detail page applies one preset to multiple selected devices and
  can clear device overrides before queuing reload commands

The current dashboard editor is table/form based. It is useful for editing
widgets, SignalK paths, font sizes, screens, and tile row/column assignments,
but it is not yet a visual layout builder. The planned visual builder should
keep the same generated dashboard schema while adding:

- a device-size preview canvas based on registered display geometry
- a widget palette for supported widget types
- drag/drop placement and direct tile resize/reorder
- a SignalK path picker instead of free-text path entry
- role/display templates for common helm, cabin, nav, and engine pages
- validation for missing widgets, duplicate tile positions, unsupported widget
  types, and layouts that do not fit the target display
- import/export compatibility with the existing `espdisp.dashboard.v1`
  preset format

The detailed builder scope, implementation outline, and documentation critique
are in [SignalK ESP Display Manager](../docs/signalk-espdisp-manager.md#visual-layout-builder).

Presets are implemented with the plugin profile store. A device's generated
dashboard config is:

```text
selected preset/profile
  + per-device overrides
  + display/layout/widget variant selection
  + unsupported widget filtering
  + font-size resolution against device capabilities
```

Device-local dashboard config endpoints are available on the ESP web UI for
bench work:

```text
GET/PUT /api/dashboard/config.json
GET/PUT /api/dashboard/config.yaml
GET     /api/security
```

The device `.yaml` endpoint intentionally uses JSON-compatible YAML syntax.
Full block-style YAML import/export is handled by the SignalK plugin.

Implemented v1 endpoints:

```text
GET  /plugins/espdisp-manager/.well-known/espdisp-management
GET  /plugins/espdisp-manager/capabilities
GET  /plugins/espdisp-manager/dashboard
GET  /plugins/espdisp-manager/discovery/devices
POST /plugins/espdisp-manager/discovery/devices
GET  /plugins/espdisp-manager/devices
GET  /plugins/espdisp-manager/groups
GET  /plugins/espdisp-manager/provisioning/tokens
POST /plugins/espdisp-manager/provisioning/tokens
POST /plugins/espdisp-manager/devices/register
GET  /plugins/espdisp-manager/devices/:id
PATCH /plugins/espdisp-manager/devices/:id
GET  /plugins/espdisp-manager/devices/:id/auth/status
POST /plugins/espdisp-manager/devices/:id/profile
POST /plugins/espdisp-manager/devices/:id/status
GET  /plugins/espdisp-manager/devices/:id/config
GET  /plugins/espdisp-manager/profiles
POST /plugins/espdisp-manager/profiles
GET  /plugins/espdisp-manager/profiles/:id/dashboard.json
GET  /plugins/espdisp-manager/profiles/:id/dashboard.yaml
POST /plugins/espdisp-manager/profiles/import-dashboard
POST /plugins/espdisp-manager/devices/:id/command
GET  /plugins/espdisp-manager/devices/:id/commands
GET  /plugins/espdisp-manager/devices/:id/commands/:commandId
POST /plugins/espdisp-manager/devices/:id/commands/:commandId/ack
POST /plugins/espdisp-manager/devices/:id/commands/:commandId/cancel
POST /plugins/espdisp-manager/devices/:id/tokens/rotate
POST /plugins/espdisp-manager/devices/:id/tokens/revoke
POST /plugins/espdisp-manager/groups/:groupId/command
POST /plugins/espdisp-manager/automation/event
GET  /plugins/espdisp-manager/firmware/catalog
POST /plugins/espdisp-manager/firmware/artifacts
GET  /plugins/espdisp-manager/firmware/artifacts/:artifactId
GET  /plugins/espdisp-manager/firmware/download/:jobId
GET  /plugins/espdisp-manager/devices/:id/firmware/jobs
POST /plugins/espdisp-manager/devices/:id/firmware/jobs
GET  /plugins/espdisp-manager/devices/:id/firmware/jobs/:jobId
POST /plugins/espdisp-manager/devices/:id/firmware/jobs/:jobId/progress
POST /plugins/espdisp-manager/devices/:id/firmware/confirm
```

Discovery announcements are a lightweight bridge for mDNS scanners, future
firmware provisioning, or test fixtures. They do not claim/register the device;
they only populate the Discovery UI and mark whether the same device id is
already in the registry.

Device registration may include display geometry and widget capabilities. The
plugin uses those fields to select layout and widget variants.

The manager control-plane benchmark and async persistence design are documented
in [SignalK Manager Load and Async Design](../docs/architecture/signalk-manager-load-and-async.md).

```json
{
  "display": {
    "width": 480,
    "height": 480,
    "rotation": 0,
    "colorDepth": 16,
    "shape": "square"
  },
  "capabilities": {
    "widgets": {
      "numeric": true,
      "text": true,
      "map": false
    },
    "fonts": {
      "sizes": [12, 14, 16, 18, 24, 32, 42, 48]
    }
  }
}
```

Profiles can provide `layout.variants` and `widgets.variants` keyed by
display match rules. Generated configs contain only the selected layout plus
supported widgets, with font sizes resolved to the device's supported font
list.

Profiles may also include a `match` object. On first registration, the plugin
assigns the highest-priority matching profile before falling back to `default`:

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

If unsupported widgets are filtered out, layout tiles referencing those widget
ids are removed from the generated config before the device sees it.
