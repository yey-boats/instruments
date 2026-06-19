# Boat Setup

This document is for using ESPDISP on a real boat network.  The development lab
topology with `nav-server`, `yey-net`, the boat simulator, static leases, and test
AP routing is separate and documented in [Lab topology](lab-topology.md).

ESPDISP is not a certified navigation instrument.  Treat it as an auxiliary
display for SignalK data and keep primary navigation, depth, AIS, engine, and
alarm systems available through approved equipment.

## Expected Boat Network

Use the boat's existing trusted LAN instead of the lab AP:

```text
              Internet / marina WiFi / LTE / Starlink
                              |
                              v
                   +----------------------+
                   | Boat router / AP     |
                   | 192.168.x.1          |
                   | DHCP + firewall      |
                   +----------+-----------+
                              |
              boat WiFi / Ethernet LAN
                              |
          +-------------------+--------------------+
          |                                        |
          v                                        v
+----------------------+              +--------------------------+
| SignalK server       |              | ESP32-S3 touch display   |
| Raspberry Pi /       |              | ESPDISP firmware         |
| mini-PC / chart PC   |              | WiFi client              |
| :3000 SignalK        |              | OTA :3232                |
| :10110 NMEA TCP opt. |              | Web diagnostics :80      |
+----------------------+              +--------------------------+
```

Recommended baseline:

- One boat router or AP provides the normal onboard WiFi.
- A SignalK server runs on a Raspberry Pi, mini-PC, chart computer, or other
  always-on Linux host.
- ESP displays join that same trusted boat WiFi as clients.
- The SignalK server has a stable hostname or reserved DHCP address.
- OTA is used only on the trusted LAN or while physically aboard.

Do not use the development `yey-net` AP, `yey-boats-sim`, or lab static routes
for normal onboard operation.

## Install SignalK

Run a normal SignalK server on the boat computer.  The firmware needs:

- SignalK HTTP/WebSocket reachable from the display, usually TCP `3000`.
- Navigation/environment paths populated by real boat sources.
- Optional NMEA 0183 TCP output on `10110` if other instruments need it.
- Optional `espdisp-manager` plugin if you want centralized display presets,
  registry, command queues, and firmware catalog workflows.

For a simple first onboard setup, configure the ESP display directly with the
SignalK host and skip the manager plugin until the base data display is stable.

## Use CI-Built Artifacts

Use artifacts produced by this repository's GitHub Actions workflows for boat
installs.  Local PlatformIO builds are for development and board bring-up.

Preferred stable path:

- Download firmware and plugin assets from the latest GitHub release:
  https://github.com/yey-boats/instruments/releases
- Release firmware is built from `release-*` PlatformIO environments. Those
  builds keep the board identity but disable debug/test controls such as touch
  injection, bench/fps/demo commands, and stall forensic logging.
- Use the target-specific merged image for first USB flashing, such as
  `esp32-4848s040-merged_firmware.bin` or
  `waveshare-touch-lcd-7b_1024x600-merged_firmware.bin`.
- Use `signalk-espdisp-manager-<version>.tgz` for the SignalK plugin.
- Verify downloads with the release `SHA256SUMS` file.

Latest-main path:

- Open the latest successful CI run on `main`.
- Download `firmware-<platformio-env>-latest` for the physical board.
- Download `signalk-espdisp-manager-<git-sha>` for the matching plugin package.
- Treat these as development or pre-release artifacts unless you have tested
  that exact build on your hardware.

Firmware CI artifacts currently contain:

- `firmware.bin`
- `firmware.elf`
- `bootloader.bin`
- `partitions.bin`
- `merged_firmware.bin`

Release assets use target-prefixed names, for example
`esp32-4848s040-merged_firmware.bin`, so multiple board builds can ship in the
same release.

## Flash Firmware

First flash from a release asset:

```sh
esptool.py --chip esp32s3 --port /dev/cu.usbserial-* write_flash 0x0 <target>-merged_firmware.bin
```

For manual flashing from individual image files, use the included bootloader,
partition table, and app image with the board's expected ESP32-S3 offsets:

```sh
esptool.py --chip esp32s3 --port /dev/cu.usbserial-* write_flash \
  0x0 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin
```

After the device joins boat WiFi and registers with SignalK, use the SignalK
ESP Display Manager firmware catalog for normal software upgrades. The manager
refreshes release assets from GitHub, matches the target-specific merged image
to the registered board id, and sends a `firmware.update` command containing
the release URL and SHA-256 checksum. `make ota` is a development workflow for
local builds, not the preferred onboard upgrade path.

Keep at least one known-good USB recovery path available before testing OTA
changes on a mounted panel.

Use firmware channels conservatively:

- `stable`: normal boat updates from tagged releases with `SHA256SUMS`.
- `prerelease` / `beta`: short lab or sea-trial runs with USB recovery nearby.
- `dev` / `lab`: local development only; do not leave these installed for
  normal navigation.

## Install The SignalK Plugin

Install the CI/release-built plugin package on the boat SignalK server:

```sh
cd ~/.signalk
npm install /path/to/signalk-espdisp-manager-<version-or-sha>.tgz
```

Restart SignalK and enable **ESP Display Manager** in the SignalK admin plugin
UI.  The plugin package should come from the same release or CI run as the
firmware when you are testing manager/device interactions.

## Provision WiFi And SignalK

From the serial console after first flash:

```text
wifi <boat-ssid> <boat-wifi-password>
sk <signalk-host-or-ip> 3000
```

Examples:

```text
wifi MyBoatLAN correct-horse-battery-staple
sk signalk.local 3000
sk 192.168.50.10 3000
```

The device reboots after saving connection settings.  Watch serial logs or use
the device web/status endpoints to confirm:

- It receives a boat-LAN IP address.
- It connects to the configured SignalK server.
- Navigation data age stays fresh while the SignalK server is receiving real
  boat data.

## Data Sources

The display renders whatever SignalK publishes.  Typical source chain:

- GPS, depth, wind, battery, tanks, engine, AIS, or autopilot devices feed the
  SignalK server through NMEA 0183, NMEA 2000, USB, serial, TCP, or plugins.
- SignalK normalizes those values into paths such as
  `navigation.speedOverGround`, `environment.depth.belowTransducer`, and
  `environment.wind.angleApparent`.
- ESPDISP subscribes to SignalK WebSocket deltas and updates its screens.

If a screen shows unavailable data, inspect SignalK first.  The firmware cannot
invent missing paths.

## Security

Use a trusted boat LAN:

- Put ESP displays and the SignalK server behind the boat router firewall.
- Avoid exposing SignalK, OTA, or device web endpoints directly to marina WiFi
  or the public internet.
- Use SignalK authentication for operator/plugin routes.
- Treat BLE and local web diagnostics as setup/troubleshooting surfaces, not
  public interfaces.
- Rotate WiFi and SignalK tokens after sharing credentials during installation.
- Use per-device manager tokens for normal managed operation. The manager
  stores device/provisioning tokens as hashes and only shows plaintext tokens
  at issue or rotation time.
- Treat `dev-shared-token` manager mode as a lab-only shortcut.

For untrusted or shared networks, use a separate onboard AP/VLAN for instruments
and allow only the routes/ports the boat actually needs.

Do not put displays directly on public marina WiFi. If a marina network is the
only WAN path, keep SignalK and displays behind an onboard router/VLAN and let
only the router talk upstream.

## Manager Plugin On A Boat

The `espdisp-manager` plugin is useful once several panels need shared presets
or remote configuration.  On a boat deployment it should run on the boat's
SignalK server, not on the development lab stack.

Suggested rollout:

1. Flash and provision the display directly.
2. Confirm live SignalK data on the display.
3. Install/enable the manager plugin on the boat SignalK server.
4. Let the display register or configure its manager endpoint.
5. Apply presets by geometry/role rather than editing device JSON by hand.

Avoid storing WiFi passwords or bearer tokens in exported presets.

## Onboard Checklist

Before relying on the display underway:

- Board boots cleanly from the intended 5 V or board-specific power supply.
- Backlight is readable in day mode and dim enough in night mode.
- Touch coordinates match the screen orientation.
- SignalK host/IP is stable after router reboot.
- Data freshness warnings are visible when SignalK is offline.
- MOB/alarm controls are reachable but cannot be triggered accidentally.
- OTA works from a trusted maintenance device.
- USB recovery remains possible after mounting.
- The display is mechanically secured and protected from spray, glare, heat,
  vibration, and accidental cable strain.

## Lab Versus Boat

Use the lab setup for repeatable development:

- `make demo-up`
- `make demo-up-remote`
- `yey-boats-sim` (boat simulator)
- `docs/lab-topology.md`
- `yey-net` SSID and `10.42.0.0/24`

Use the boat setup for real operation:

- Real boat router/AP
- Real SignalK server and real sensors
- Normal boat LAN addressing
- Direct firmware provisioning or the manager plugin on the boat SignalK server

Keep these separate so development routing, fake data, and test credentials do
not leak into the onboard configuration.
