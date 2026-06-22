# LAB-ONLY: temporary web-auth bypass (`YEYBOATS_LAB_OPEN_WEB`)

**Status:** temporary lab tooling. **Never ship a build that defines this flag.**

## Why this exists

The device web API (`/api/state`, `/api/diag`, `/api/screenshot.png`, `/api/cmd`,
…) is gated by HTTP Basic Auth (`require_api_auth()` in `src/web.cpp`). The
credentials live in the device's NVS (`web/{auth,user,pass}`) and are set by the
manager — they are **not** in this repo. To capture headless screenshots for
render verification on a bench device whose web password is unknown, we need a
way to open the API without knowing or changing that password.

## What it does

`require_api_auth()` has a compile-time branch:

```cpp
#ifdef YEYBOATS_LAB_OPEN_WEB
    return true;   // bypass: every /api/* request is allowed
#else
    /* normal Basic Auth */
#endif
```

It is **off by default** — production envs do not define `YEYBOATS_LAB_OPEN_WEB`,
so the branch compiles out entirely and Basic Auth is unchanged. It only affects
HTTP web-API auth; it does **not** touch the device-token (manager) path, OTA
(separate password), or BLE/serial.

It does **not** modify NVS. The device's real `web/{auth,user,pass}` are left
intact, so re-securing is just reflashing a normal build.

## Use (bench device via the lab relay)

```sh
# Build + OTA a bypass image (flag appended via PLATFORMIO_BUILD_FLAGS):
PLATFORMIO_BUILD_FLAGS="-D YEYBOATS_LAB_OPEN_WEB=1" \
  make ota-verify DEVICE_IP=<device-ip> REMOTE=<user@relay>

# Now the API is open — e.g. through the relay:
ssh <user@relay> 'curl -s http://<device-ip>/api/screenshot.png' > shot.png
ssh <user@relay> 'curl -s -X POST --data "midl-render" http://<device-ip>/api/cmd'
ssh <user@relay> 'curl -s -X POST --data "screen midl"  http://<device-ip>/api/cmd'
```

## RE-SECURE WHEN DONE (required)

Reflash a normal production image (no flag) to restore Basic Auth:

```sh
make ota-verify DEVICE_IP=<device-ip> REMOTE=<user@relay>
```

The original NVS password still applies after reflashing. Confirm with
`curl -o /dev/null -w '%{http_code}' http://<device-ip>/api/state` → expect `401`.

## Checklist

- [ ] Bypass image flashed only to the **bench/lab** device, never a customer unit.
- [ ] Verification screenshots captured.
- [ ] **Production image reflashed (auth re-enabled), 401 confirmed.**
