# Security policy

## Supported versions

Active development happens on `main`. Tagged releases are supported for
six months after release; security fixes are backported only to the latest
release at the maintainer's discretion.

## Reporting a vulnerability

**Please do not file a public GitHub issue for security problems.**

Use the repository's [private security advisory](https://github.com/navado/esp32-boat-mfd/security/advisories/new)
to report the vulnerability. Include:

- A description of the issue and the impact
- Steps to reproduce or a proof of concept
- The affected commit / release
- Any suggested mitigation

Expected response time: 7 days for an initial acknowledgement, 30 days
for a fix or coordinated disclosure plan.

## Known sharp edges

This is firmware for a development-class device. By default:

- ArduinoOTA runs **without a password** (set `OTA_PASSWORD` in `secrets.h`).
- BLE GATT accepts commands with **no pairing** (anyone in BLE range can
  rewrite the WiFi credentials).
- WiFi credentials are stored in NVS in plaintext.
- The SignalK WebSocket connection is **unencrypted** (`ws://`, not `wss://`).

These are appropriate for hobby / boat-local use but **not** for fleet
deployment. Hardening for production deployment is on the roadmap.
