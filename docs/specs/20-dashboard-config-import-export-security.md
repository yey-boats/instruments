# Dashboard Configuration Import/Export And Access Security

## Goal

Allow ESP display dashboards to be configured centrally from SignalK, exported
for review/version control, imported back into SignalK or a device, and applied
to devices without exposing raw generated config editing as the normal operator
workflow.

## Dashboard Document

Canonical SignalK import/export document:

```yaml
kind: espdisp.dashboard.v1
preset:
  id: helm-day
  name: Helm Day
  version: 1
dashboard:
  settings:
    defaultScreen: dashboard
    theme: day
    brightness: 0.85
  nmea0183Wifi:
    enabled: true
    mode: tcp
    host: signalk.local
    port: 10110
  widgets:
    defaults:
      fontSize: 18
      labelFontSize: 12
      valueFontSize: 48
      unitFontSize: 16
    items:
      sog:
        type: numeric
        title: SOG
        path: navigation.speedOverGround
        unit: kn
  layout:
    screens:
      - id: dashboard
        type: grid
        tiles:
          - widget: sog
```

JSON is the canonical machine format. YAML is for human review and sharing.
SignalK exports block-style YAML and imports the same limited YAML profile.
The device exports and imports JSON-compatible YAML only: JSON syntax served
with `application/yaml`. This keeps the ESP32 parser deterministic and avoids a
large YAML dependency in firmware.

## SignalK Plugin Contract

Preset dashboard export:

```text
GET /plugins/espdisp-manager/profiles/:id/dashboard.json
GET /plugins/espdisp-manager/profiles/:id/dashboard.yaml
```

Preset dashboard import:

```text
POST /plugins/espdisp-manager/profiles/import-dashboard
Content-Type: application/json | application/yaml | text/plain
```

The import creates or updates a preset/profile. The operator can then apply it
to one or more devices from the preset detail UI and optionally queue
`config.reload`.

## Device Web Contract

Device-local dashboard import/export aliases:

```text
GET /api/dashboard/config.json
PUT /api/dashboard/config.json
GET /api/dashboard/config.yaml
PUT /api/dashboard/config.yaml
```

These endpoints are aliases for the existing layout document consumed by
`/api/layout`. The root device web page exposes a dashboard configuration
textarea with export JSON/YAML and import/apply controls.

## BLE Contract

The existing boat-mfd CONFIGURATION characteristic carries the same dashboard
layout document. A single GATT read/write is limited to 512 bytes. Larger
dashboard imports should use the device web endpoint or SignalK manager.

## Access Security

### SignalK

- SignalK HTTP auth protects operator routes.
- Device pull access uses `X-EspDisp-Authorization: Bearer <device-token>`.
- Generated configs do not contain WiFi passwords or SignalK bearer tokens.
- Preset import/export should be treated as operational config, not a secret.

### Device Web

- Current firmware exposes device web endpoints without on-device HTTP auth.
- Intended use is trusted LAN or temporary setup AP only.
- The device never echoes WiFi passwords or SignalK tokens.
- HTTP touch/gesture injection remains blocked; those commands are BLE/serial
  only.
- `/api/security` reports the active access model and writable endpoints.

### BLE

- Current firmware does not require BLE pairing or application-layer auth.
- BLE configuration is intended for local physical proximity during setup.
- BLE writes can configure WiFi, SignalK target, screen selection, and small
  dashboard documents.
- Future hardening should add a provisioning PIN or signed manager claim before
  enabling persistent BLE configuration writes.

## Acceptance

- SignalK can export a dashboard preset as JSON and YAML.
- SignalK can import a dashboard preset from JSON and YAML.
- The SignalK UI links preset JSON/YAML exports and has an import form.
- The device web UI can export/import dashboard config JSON.
- The device web UI exposes a YAML endpoint using JSON-compatible YAML.
- Device access security is documented and discoverable through `/api/security`.
