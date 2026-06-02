# espdisp lab UDP log listener

Captures the firmware's `logf()` UDP broadcasts on the lab box and
rotates them weekly. Receives only when the device runs a debug build
(`pio -e esp32-4848s040-debug`, gated by `-D ESPDISP_DEBUG_UDP_LOG=1`).
Release builds never broadcast.

## Components

- `loglistener.py` — stdlib-only UDP listener. Prints `ISO-ts [src-ip] line` to stdout.
- `espdisp-loglistener.service` — systemd unit. `DynamicUser=yes`, sandboxed, restarts on failure. stdout goes to `/var/log/espdisp/device.log` via `StandardOutput=append:`.
- `espdisp.logrotate` — daily rotation, 7-day retention, gzip-compressed, `copytruncate` (listener has no SIGHUP).
- `install.sh` — idempotent root installer; copies files into place, reloads systemd, enables + restarts the service, validates logrotate config.

## Deploy (from the dev box)

```sh
make lab-logger-deploy REMOTE=compulab@192.168.2.11
```

Under the hood: tars `tools/lab-logger/`, ships it via SSH, runs `install.sh` with sudo.

## Verify

```sh
ssh compulab@192.168.2.11 'sudo tail -f /var/log/espdisp/device.log'
# or
ssh compulab@192.168.2.11 'sudo journalctl -u espdisp-loglistener -f'
```

If the file stays empty: confirm the device is running the debug firmware (`make ota ENV=esp32-4848s040-debug DEVICE_IP=<ip>`) and on the same broadcast domain as the lab box (the lab AP `wlan-ap0` on `compulab` puts devices on `10.42.0.0/24`).

## Why UDP broadcast?

The device already has BLE NUS log notifications (single consumer) and an HTTP `/api/logs` ring buffer (poll-based, drops between samples). UDP broadcast on the LAN is fire-and-forget and any host can subscribe — perfect for a stationary lab collector. The wire format is intentionally trivial: one datagram per `logf()`, raw text, no framing.

## Wire format

```
<src-broadcast>:9999  -->  one datagram per logf() line, UTF-8, optional trailing '\n'
```

Listener decoration:

```
2026-06-02T17:23:11.482  [10.42.0.67]  [sk] STALL begin connected=1 ...
```
