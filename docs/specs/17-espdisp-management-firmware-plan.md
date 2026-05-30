# ESP Display Management Firmware Spec Plan

Status: draft plan.

## Goal

Add firmware support for centralized SignalK-side device management while
keeping the display usable with local configuration and current SignalK data
flows.

The firmware should be able to:

- discover the management service
- register with stable identity and capabilities
- authenticate with a device token
- report status and diagnostics
- fetch and apply central config
- execute queued commands
- report firmware and OTA metadata
- support future pull-based OTA updates

## Spec Documents To Produce

### 1. Device Identity And Capabilities

Define stable identity fields:

- `deviceId`
- MAC address
- board id
- display/touch controller
- chip, flash, PSRAM
- firmware version/build/git metadata
- supported features

Define capability flags:

- touch
- touch interrupt
- BLE config
- ArduinoOTA
- pull OTA
- NMEA 0183 WiFi
- NMEA 2000
- autopilot controls
- audio/beeper
- local web UI

Firmware implementation target:

```text
include/device_identity.h
src/device_identity.cpp
```

Default identity:

- new devices use `espdisp-<12 lowercase hex WiFi STA MAC>` as `deviceId`,
  BLE name, mDNS host, and OTA host
- custom IDs set with `id <name>` are preserved in NVS
- `id auto` restores the hardware-derived default
- legacy blank, `espdisp`, and `espdisp-device` defaults migrate to the
  hardware-derived ID on boot

### 2. Management Endpoint Discovery

Define discovery order:

1. stored manager endpoint from NVS
2. mDNS `_espdisp-mgmt._tcp.local`
3. existing SignalK host plus plugin well-known path
4. manual serial/BLE configuration

Define persisted fields:

- manager host
- manager port
- base path
- TLS flag
- last successful discovery method

Console commands:

```text
manager-status
manager-register <host> [port]
manager-token <token>
manager-forget
manager-discover
```

### 3. Device Authentication

Define two firmware auth states:

```text
unprovisioned
provisioned
```

Unprovisioned calls use:

```text
Authorization: EspDisp-Provision <setup-token>
```

Provisioned calls use:

```text
Authorization: Bearer <device-token>
```

NVS storage:

- device token
- token id, if provided
- provisioned flag

Rules:

- never log full tokens
- allow token replacement
- allow factory reset
- treat 401/403 as management disconnected, not as fatal UI failure

### 4. Registration

Define boot registration request and response.

Registration should happen:

- after WiFi is connected
- after management endpoint is discovered
- before config fetch
- periodically if the plugin asks for re-registration

Expected response fields:

- device token, when provisioning succeeds
- heartbeat interval
- command poll interval
- config URL
- current claim/profile state

### 5. Status Heartbeat

Define heartbeat cadence and payload.

Required status groups:

- network
- SignalK connection
- UI state
- touch state
- NMEA WiFi state
- NMEA 2000 state
- memory
- firmware
- OTA listener
- config version/hash
- recent errors

Response handling:

- server time
- desired config version/hash
- command count
- requested re-registration

### 6. Central Config Fetch And Apply

Define device config schema consumed by firmware.

Config sections:

- management
- network identity
- SignalK
- source priority/timeouts
- NMEA 0183 WiFi
- NMEA 2000
- layout
- theme
- brightness
- autopilot permissions
- debug
- OTA metadata

Apply rules:

- validate before apply
- apply in dependency order
- persist only accepted config
- report config apply result in heartbeat
- fallback to previous config on invalid update

Dependency order:

1. network hostname/domain
2. SignalK target
3. source settings
4. layout/theme/UI settings
5. debug settings

### 7. Hostname And mDNS Management

Define firmware behavior for:

- desired hostname
- desired domain
- FQDN
- current hostname
- mDNS advertisement

Advertise services:

```text
_espdisp._tcp
_arduino._tcp
```

Rules:

- apply hostname before WiFi reconnect where possible
- restart mDNS after hostname change
- report conflict/error if hostname cannot be applied
- derive OTA address from FQDN when available

### 8. Command Polling And Execution

Define command poll loop:

```text
GET /devices/:id/commands?limit=10
```

Define command ack:

```text
POST /devices/:id/commands/:commandId/ack
```

Supported v1 commands:

- `screen.set`
- `theme.set`
- `brightness.set`
- `layout.reload`
- `config.reload`
- `beep`
- `overlay.show`
- `overlay.clear`
- `touch.mode`
- `log.level`
- `reboot`
- `firmware.update`

Ack result codes:

- `ok`
- `unsupported_command`
- `invalid_payload`
- `busy`
- `failed`

### 9. Firmware Metadata

Define build constants:

- firmware name
- semantic version
- build time
- git commit
- git dirty flag
- PlatformIO env
- board id

Define partition metadata:

- running partition
- next OTA partition
- rollback supported
- pending confirm

### 10. OTA Support

v1 reports ArduinoOTA metadata:

- enabled
- mode
- address/FQDN
- port
- password set

v2 implements pull OTA:

- receives `firmware.update`
- validates artifact metadata
- streams binary through ESP update API
- calculates SHA-256 during download
- reports progress
- reboots
- confirms successful boot

Progress states:

- accepted
- downloading
- verifying
- installing
- rebooting
- booted
- confirmed
- failed
- rolled back

### 11. Local Diagnostics

Add diagnostics to existing serial/BLE/web surfaces:

- manager endpoint
- auth state
- last register status
- last heartbeat status
- config version/hash
- pending command count
- last command result
- firmware update state

## Firmware Milestones

### Milestone F1: Identity And Status

- add identity module
- add firmware build metadata
- add management status shell commands
- no server dependency yet

### Milestone F2: Register And Heartbeat

- discover configured management endpoint
- register with plugin
- persist device token
- send heartbeat
- expose status in logs and web API

### Milestone F3: Config Fetch

- fetch device config
- validate version/hash
- apply SignalK, source priority, theme, and layout settings
- persist accepted config

### Milestone F4: Commands

- poll commands
- implement non-destructive command handlers
- ack command results

### Milestone F5: Network Identity

- apply hostname/domain config
- advertise mDNS services
- report current FQDN and OTA address

### Milestone F6: Firmware Management

- report partition metadata
- report ArduinoOTA state
- implement pull OTA progress reporting
- implement pull OTA install path

## Open Firmware Questions

- How much JSON can be safely parsed in one config payload?
- Should command polling use HTTP first and WebSocket later?
- Should device token be stored in NVS plain text or encrypted where platform
  support exists?
- Which commands require confirmation or physical interaction?
- Should autopilot engage commands be disabled by default in managed config?
