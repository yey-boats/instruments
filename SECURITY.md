# Security policy

## Supported versions

Active development happens on `main`. Tagged releases are supported for
six months after release; security fixes are backported only to the latest
release at the maintainer's discretion.

## Reporting a vulnerability

**Please do not file a public GitHub issue for security problems.**

Use the repository's [private security advisory](https://github.com/yey-boats/instruments/security/advisories/new)
to report the vulnerability. Include:

- A description of the issue and the impact
- Steps to reproduce or a proof of concept
- The affected commit / release
- Any suggested mitigation

Expected response time: 7 days for an initial acknowledgement, 30 days
for a fix or coordinated disclosure plan.

## Known sharp edges

This is firmware for a development-class device. Local builds still default to
empty secrets unless `include/secrets.h` or `YEYBOATS_OTA_PASSWORD` is provided.
CI firmware builds read `YEYBOATS_OTA_PASSWORD` from GitHub Secrets when it is
available, and tagged release builds require that secret before producing
firmware artifacts.

- ArduinoOTA runs **without a password** if `OTA_PASSWORD` is empty. Set
  `YEYBOATS_OTA_PASSWORD` before `make build`/`make ota`, or configure the
  GitHub Actions secret `YEYBOATS_OTA_PASSWORD` for CI and release builds.
- BLE GATT accepts commands with **no pairing** (anyone in BLE range can
  rewrite the WiFi credentials).
- WiFi credentials are stored in NVS in plaintext.
- The SignalK WebSocket connection is **unencrypted** (`ws://`, not `wss://`).

An OTA password embedded in firmware is still present in the binary. Treat
public release assets as disclosing that value and rotate the secret if the
asset has been shared outside the intended deployment group.

These are appropriate for hobby / boat-local use but **not** for fleet
deployment. Hardening for production deployment is on the roadmap.
