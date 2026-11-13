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

- ArduinoOTA (espota, port 3232): see "Firmware updates" below. Release
  builds do not include it; dev builds run it only with a password.
- BLE GATT accepts commands with **no pairing** (anyone in BLE range can
  rewrite the WiFi credentials).
- WiFi credentials are stored in NVS in plaintext.
- The SignalK WebSocket connection is **unencrypted** (`ws://`, not `wss://`).

## Web API credentials (instruments#21)

The device web API (`/api/*`) uses HTTP Basic Auth. There is **no built-in or
shared default password** in the firmware:

1. **Factory / setup state - open.** A device with no web password in NVS
   (`web/pass`) serves the API without auth so first-boot provisioning (setup
   AP captive page, BLE, on-screen WiFi setup) works. `/api/security` reports
   `web.auth = "setup-open"`.
2. **Set up - Basic Auth on by default.** Once a password is provisioned
   (today: the SignalK manager pushes `webAuth.{username,password}` when it
   pairs the device), Basic Auth is enforced. The NVS `web/auth` flag now
   defaults to *on* when absent; `web.auth = "basic"`. Username defaults to
   `espdisp`.
3. **Owner opt-out.** Only an explicit `webAuth.enabled=false` disables it
   (`web.auth = "disabled"`).

Lost password: re-pair from the manager (it re-pushes its credentials), or
erase NVS / factory-reset over USB.

**Stored as a salted hash (instruments#30).** The password is kept in NVS as a
PBKDF2-HMAC-SHA256 record (`web/pass_h`: `pbkdf2-sha256$<iterations>$<salt>$<key>`).
Basic Auth decodes the presented credential, derives a key with the stored
salt, and compares in constant time. Basic Auth still travels over plain HTTP,
so the credential is only as private as the LAN.

| Parameter | Value |
|---|---|
| KDF | PBKDF2-HMAC-SHA256 (mbedtls, ESP32-S3 HW SHA) |
| Iterations | 1000 (stored per record, so it can be raised later) |
| Salt | 16 bytes, `esp_fill_random`, new on every password change |
| Derived key | 32 bytes |
| Cost per derive | ~20-30 ms estimated on ESP32-S3 at 240 MHz (~4000 SHA-256 compressions); the device logs the measured `pbkdf2-sha256 x1000 took N ms` on its first derive |
| Verify cache | the last successful credential, as an HMAC-SHA256 tag (per-boot random key) over record + password, in RAM only, 30 s TTL; constant-time compare; cleared on any password change; failures never cached |

So a client polling the API pays one PBKDF2 derive per 30 s, and every wrong
guess still pays a full derive.

**Migration and rollback.** On the first boot of this firmware, a plaintext
`web/pass` left by older firmware is hashed into `web/pass_h`. While the new
image is still `PENDING_VERIFY` the plaintext is kept, so a bootloader rollback
to the old firmware still finds its password. A password set in that window is
written to both keys. Once `manager::setup` marks the image valid
(`esp_ota_mark_app_valid_cancel_rollback`), the plaintext is erased. If the
record cannot be written, the plaintext stays and still authenticates (fails
closed).

**Manual downgrade.** Flashing firmware older than this change after the
plaintext has been erased leaves that firmware with no password it can read,
so its API is setup-open until the manager pushes the config (password) again.
After any manual downgrade below this version, re-push the device config from
the manager.

## Peer control `/api/p2p/*`

`/api/p2p/*` uses the control-protocol shared key (`ctl key <secret>`, NVS
`proto/key`), not Basic Auth. An empty key still means open; the key compare
is now constant-time. Requiring the key once a device is paired needs the
manager to deliver keys first and is tracked in instruments#36.

## Firmware updates (instruments#30)

- **Release builds** (`release-*`, `YEYBOATS_RELEASE_BUILD`) compile ArduinoOTA
  out, so nothing listens on 3232. Updates go through the manager's pull OTA:
  the device polls `/devices/<id>/firmware/jobs` with its device token,
  downloads the image, checks its sha256, and flashes the inactive slot. USB
  flashing also works.
- **Dev builds** start ArduinoOTA only when a password is set. That is the
  runtime one (`ota-pass <pw>` over serial/BLE, or the manager's
  `ota.password`) or the compile-time `OTA_PASSWORD` (`YEYBOATS_OTA_PASSWORD`).
  With no password, ArduinoOTA stays off. `ota-pass <pw>` starts it without
  a reboot.
- **Lab only:** a password-less ArduinoOTA needs an explicit
  `-D YEYBOATS_LAB_OPEN_OTA=1` (`make ota LAB_OPEN_OTA=1`). No env sets it.
  `net.cpp` refuses to compile it with `YEYBOATS_RELEASE_BUILD=1`, and CI
  (`tools/test_web_security.py`) checks that no env or workflow defines it.

The web server sends **no CORS headers** (`Access-Control-Allow-Origin` was
`*`). The on-device UI is same-origin and the manager/layout editor reach the
device server-side through the plugin, so no browser needs cross-origin access.

The lab-only `YEYBOATS_LAB_OPEN_WEB` auth bypass is refused at compile time in
any `YEYBOATS_RELEASE_BUILD=1` build, and CI (`tools/test_web_security.py`) asserts no
release env or workflow defines it.

An OTA password embedded in firmware is still present in the binary. Treat
public release assets as disclosing that value and rotate the secret if the
asset has been shared outside the intended deployment group.

These are appropriate for hobby / boat-local use but **not** for fleet
deployment. Hardening for production deployment is on the roadmap.
