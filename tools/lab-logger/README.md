# espdisp lab UDP log listener

Captures the firmware's `logf()` UDP broadcasts on the lab box and
rotates them weekly. Receives only when the device runs a debug build
(`pio -e esp32-4848s040-debug`, gated by `-D YEYBOATS_DEBUG_UDP_LOG=1`).
Release builds never broadcast.

## Components

- `loglistener.py` — stdlib-only UDP listener. Prints `ISO-ts [src-ip] line` to stdout.
- `yeydisp-loglistener.service` — systemd unit. `DynamicUser=yes`, sandboxed, restarts on failure. stdout goes to `/var/log/yeydisp/device.log` via `StandardOutput=append:`.
- `espdisp.logrotate` — daily rotation, 7-day retention, gzip-compressed, `copytruncate` (listener has no SIGHUP).
- `install.sh` — idempotent root installer; copies files into place, reloads systemd, enables + restarts the service, validates logrotate config.

## Deploy (unattended)

```sh
make lab-logger-deploy REMOTE=compulab@192.168.2.11
# or end-to-end (build debug FW + OTA flash + deploy logger):
make lab-up REMOTE=compulab@192.168.2.11 DEVICE_IP=10.42.0.67
```

`tools/lab-logger/deploy.sh` runs without prompts. Sudo auth in this order:

1. Remote user has passwordless sudo → just runs.
2. `REMOTE_SUDO_PASS` in env or `.env.test.local` (gitignored) → fed to `sudo -S` over SSH stdin.
3. Neither → script exits non-zero with the one-line fix. There is no interactive fallback by design.

Add to `.env.test.local` once:

```sh
echo 'REMOTE_SUDO_PASS=your-sudo-password' >> .env.test.local
chmod 600 .env.test.local
```

## Log volume controls

The firmware's UDP broadcast is gated by a runtime severity threshold + optional tag filter so the lab LAN doesn't get flooded. Default is **WARN+ only, no tag filter** — quiet enough to leave on permanently in a debug build. Widen on demand via BLE or serial:

```sh
make ble-cmd CMD="log-status"           # show current filter
make ble-cmd CMD="log-level debug"      # widen to DEBUG+
make ble-cmd CMD="log-tag sk"           # only [sk] tag passes
make ble-cmd CMD="log-tag clear"        # remove tag filter
```

Settings persist in NVS, so the device boots back into whatever filter you set. `log-level` and `log-tag` both also work over the serial console and `net::dispatchCommand`.

Severity levels: `error` (1) < `warn` (2) < `info` (3) < `debug` (4) < `trace` (5). Lines at or below the configured level pass. The SignalK stall transition lines (`STALL begin` / `STALL end`) are classified as WARN so they reach the lab logger by default — that's the "specific issue" the logger exists to capture.

## Verify

```sh
ssh compulab@192.168.2.11 'sudo tail -f /var/log/yeydisp/device.log'
# or
ssh compulab@192.168.2.11 'sudo journalctl -u yeydisp-loglistener -f'
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
