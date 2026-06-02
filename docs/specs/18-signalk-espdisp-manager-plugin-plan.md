# SignalK ESP Display Manager Plugin Spec Plan

Status: draft plan.

## Goal

Build a SignalK plugin that manages ESP display devices as first-class managed
clients.

The plugin should provide:

- device registry
- provisioning and authentication
- service discovery metadata
- central configuration profiles
- command queue
- hostname/domain assignment
- diagnostics and audit log
- firmware catalog
- OTA job tracking
- admin UI

## Spec Documents To Produce

### 1. Plugin Configuration

Define plugin config schema:

- enabled flag
- auth mode
- provisioning token settings
- heartbeat and command polling defaults
- network naming policy
- domain
- firmware storage path
- allowed vendors/channels
- development mode flags

Example sections:

```text
auth
provisioning
network
firmware
automation
ui
```

### 2. Device Registry

Define registry record:

- device id
- name
- role
- location
- claimed state
- identity
- capabilities
- firmware info
- network identity
- auth/token metadata
- assigned profile
- config version/hash
- last seen/status
- recent errors

Endpoints:

```text
GET    /devices
POST   /devices/register
GET    /devices/:id
PATCH  /devices/:id
DELETE /devices/:id
```

### 3. Provisioning And Authentication

Define auth flows:

- development shared token
- expiring provisioning token
- per-device bearer token

Endpoints:

```text
POST /provisioning/tokens
POST /devices/:id/tokens/rotate
POST /devices/:id/tokens/revoke
GET  /devices/:id/auth/status
```

Rules:

- store token hashes only (implemented for device and provisioning tokens)
- never return existing token after creation
- audit token changes
- support unclaimed device review

### 4. Service Discovery

Define well-known endpoint:

```text
GET /.well-known/espdisp-management
```

Response fields:

- protocol version
- server id
- base path/base URL
- auth methods
- feature flags
- default heartbeat interval
- default command poll interval

Define optional mDNS advertisement:

```text
_espdisp-mgmt._tcp.local
```

TXT fields:

- protocol
- path
- auth methods
- TLS flag
- SignalK HTTP port
- NMEA TCP port

### 5. Config Profiles

Define profile model:

- profile id
- name
- description
- version
- hash
- config body
- created/updated metadata

Define assignment model:

- device id
- profile id
- per-device overrides
- generated config version
- generated config hash

Endpoints:

```text
GET  /profiles
POST /profiles
GET  /profiles/:id
PUT  /profiles/:id
POST /devices/:id/profile
GET  /devices/:id/config
```

Generated config should include:

- network identity
- SignalK settings
- source settings
- layout
- theme
- NMEA WiFi
- autopilot permissions
- debug settings
- OTA metadata

### 6. Hostname And Domain Management

Define naming policy:

- `device-id`
- `role-location`
- `manual`

Define registry fields:

- desired hostname
- desired domain
- desired FQDN
- current hostname
- current FQDN
- last resolved address
- conflict state

Rules:

- detect duplicate desired hostnames
- provide deterministic fallback names
- v1 manages mDNS and hostname only
- v2 may integrate real DNS providers

Potential DNS providers for v2:

- Pi-hole
- CoreDNS
- dnsmasq
- router API

### 7. Device Status

Define status ingestion endpoint:

```text
POST /devices/:id/status
```

Plugin updates:

- online/offline state
- last seen
- health score
- config drift
- command pending count
- firmware drift
- network identity state

Response includes:

- server time
- desired config version/hash
- command count
- optional re-register request

### 8. Command Queue

Define command lifecycle:

```text
pending -> delivered -> acknowledged
pending -> delivered -> failed
pending -> expired
pending -> cancelled
```

Endpoints:

```text
POST /devices/:id/command
POST /groups/:id/command
GET  /devices/:id/commands
POST /devices/:id/commands/:commandId/ack
GET  /devices/:id/commands/:commandId
```

Command types v1:

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

### 9. Automation API

Define automation endpoint:

```text
POST /automation/event
```

Inputs:

- SignalK path changes
- notifications
- schedules
- external HTTP calls

Initial automation actions:

- send command to device
- send command to group
- assign profile
- set theme
- show overlay

Example rules:

- night mode at sunset
- MOB overlay on all devices
- autopilot screen when autopilot is active
- debug logging for test group only

### 10. Firmware Catalog

Define vendor-aware firmware artifact metadata:

- vendor id/name
- product id/name
- board id
- chip
- firmware version
- channel
- build time
- git metadata
- file size
- SHA-256
- signature metadata
- compatibility rules
- release notes
- source
- trust level

Endpoints:

```text
GET  /firmware/catalog
POST /firmware/artifacts
GET  /firmware/artifacts/:artifactId
GET  /firmware/download/:jobId
```

Storage layout:

```text
plugin-data/espdisp-manager/firmware/
  artifacts/<vendor>/<product>/<board>/<version>/
  indexes/
  jobs/
```

### 11. OTA Jobs

Define OTA job model:

- job id
- device id
- artifact id
- policy
- status
- progress
- timestamps
- result

Endpoints:

```text
POST /devices/:id/firmware/jobs
GET  /devices/:id/firmware/jobs/:jobId
POST /devices/:id/firmware/jobs/:jobId/progress
POST /devices/:id/firmware/confirm
```

Job states:

- queued
- accepted
- downloading
- verifying
- installing
- rebooting
- booted
- confirmed
- failed
- rolled_back

### 12. Admin UI

Pages:

- devices
- device detail
- profiles
- commands
- firmware catalog
- OTA jobs
- settings
- audit log

Device detail should show:

- online/offline
- current screen
- firmware version
- assigned profile
- config drift
- network hostname/FQDN/IP
- OTA address
- last command
- recent errors

### 13. Audit Log

Audit events:

- device registered
- device claimed
- token created/rotated/revoked
- profile changed
- config assigned
- command created/acknowledged/failed
- hostname changed
- firmware artifact uploaded
- OTA job created/progressed/completed

Use append-only JSONL:

```text
plugin-data/espdisp-manager/audit.jsonl
```

## Plugin Milestones

### Milestone S1: Skeleton And Storage

- create SignalK plugin package
- config schema
- plugin data directory
- basic admin UI placeholder

### Milestone S2: Registry

- registration endpoint
- device list/detail endpoints
- registry persistence
- unclaimed device handling

### Milestone S3: Auth

- provisioning tokens
- per-device tokens
- token hashing
- audit log

### Milestone S4: Status And Config

- heartbeat endpoint
- profile store
- config generation
- config hash/version
- config fetch endpoint

### Milestone S5: Commands

- command queue
- device polling
- ack handling
- group commands

### Milestone S6: Network Identity

- hostname policy
- conflict detection
- generated network identity config

### Milestone S7: Firmware Catalog

- artifact upload
- metadata validation
- vendor/product/board indexes
- compatibility checks

### Milestone S8: OTA Jobs

- firmware update jobs
- download endpoint
- progress/confirm endpoints
- audit trail

### Milestone S9: Automation And UI

- automation endpoint
- basic rule actions
- production-ready admin UI

## Open Plugin Questions

- Which SignalK plugin API is best for mDNS advertisement?
- Should device tokens reuse SignalK auth or remain plugin-local?
- Should profiles live only in plugin data or also expose SignalK resources?
- How much admin UI should ship in v1?
- Should firmware binaries be stored in plugin data or external object storage?
