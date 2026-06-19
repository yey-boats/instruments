# Lab topology

This is the layout the firmware is tested against day-to-day.  The lab has a
normal boat/home WAN router and a Docker-capable Linux mini-PC named
`nav-server`.

`nav-server` can be any small Linux machine that can run Docker and host a
2.4 GHz lab access point.  My current box is a Compulab IOT-GATE-IMX8PLUS
industrial ARM IoT gateway:
https://www.compulab.com/products/iot-gateways/iot-gate-imx8plus-industrial-arm-iot-gateway/

Configure `nav-server` as a DNS name or SSH host alias for the mini-PC before
using the default Make targets.  In this lab, `nav-server` resolves to the host
on `192.168.2.11`.

```text
                                Internet / marina uplink
                                         |
                                         v
                 +------------------------------------------------+
                 | Router / Starlink / etc. WAN router            |
                 | LAN IP: 192.168.2.1                            |
                 | Static route: 10.42.0.0/24 via 192.168.2.11    |
                 +----------------------+-------------------------+
                                        |
                         boat/home LAN  | 192.168.2.0/24
                                        |
                 +----------------------+-------------------------+
                 |                                                |
                 v                                                v
        +-------------------+                         +----------------------+
        | Dev workstation   |                         | nav-server           |
        | Mac/Linux         |                         | Linux mini-PC        |
        | 192.168.2.3       |                         | eth0 192.168.2.11   |
        | pio, pytest, BLE  |                         | Docker + SignalK    |
        +-------------------+                         | hostapd + dnsmasq    |
                                                      +----------+-----------+
                                                                 |
                                            wlan-ap0 10.42.0.1/24|
                                       SSID yey-net, WPA2, 2.4GHz|
                                                                 v
                                                      +----------------------+
                                                      | yey-net AP segment   |
                                                      | 10.42.0.0/24         |
                                                      | DHCP 10.42.0.50-150  |
                                                      +----------+-----------+
                                                                 |
                                                                 v
                                                      +----------------------+
                                                      | ESP32-S3 MFD         |
                                                      | 10.42.0.67           |
                                                      | gw 10.42.0.1         |
                                                      +----------------------+

SignalK Docker on nav-server uses host networking and exposes:
  3000/tcp  SignalK HTTP/WebSocket
  10110/tcp NMEA 0183 TCP
  34300/udp SignalK discovery responder
  34301/udp ESP display announcements
```

## Why routed instead of bridged or NAT

- **Bridged into the WAN router LAN** would be the simplest UX (mDNS works, single
  broadcast domain) but it ties `eth0` into a bridge, and any glitch
  there drops SSH to `nav-server` — recovery requires physical access.
- **NAT** is what `nmcli`'s `ipv4.method=shared` gives you out of the
  box, but then nothing on the WAN-router LAN can reach an AP-side device by
  IP, which kills OTA-from-laptop.
- **Routed** keeps `nav-server` SSH stable (eth0 untouched), gives full
  L3 reachability in both directions once the WAN router knows where the
  segment lives, and lets `tcpdump` on `wlan-ap0` see raw device
  traffic.

## What lives where

| Service             | Host         | Listens on                 |
|---------------------|--------------|----------------------------|
| SignalK server      | `nav-server` | `0.0.0.0:3000` (host net)  |
| NMEA 0183 TCP       | `nav-server` | `0.0.0.0:10110`            |
| SK discovery UDP    | `nav-server` | `0.0.0.0:34300/udp`        |
| device-announce UDP | `nav-server` | `0.0.0.0:34301/udp`        |
| `espdisp-manager`   | `nav-server` | `/plugins/espdisp-manager` |
| `yey-boats-sim`     | dev laptop   | pushes to SK over WS       |
| `hostapd`           | `nav-server` | `wlan-ap0`                 |
| `dnsmasq` (DHCP)    | `nav-server` | `wlan-ap0`                 |

## What the laptop / router side needs

The Router/Starlink/etc. WAN router needs **one** static route so OTA, `pytest`, and ad-hoc
`curl` from anywhere on `192.168.2.0/24` reach the MFD:

```
Destination: 10.42.0.0
Netmask:     255.255.255.0
Gateway:     192.168.2.11
```

If you can't (or don't want to) touch the WAN router, a per-machine
stopgap works for the dev laptop:

```sh
sudo route -n add -net 10.42.0.0/24 192.168.2.11   # macOS
sudo ip route add  10.42.0.0/24 via 192.168.2.11   # Linux
```

That route is lost on reboot — make it durable via the OS network
config when you want it permanent.

## Bring-up + tear-down

```sh
# One-time: copy `.env.test.local.example` to `.env.test.local` and fill in
# YEY_NET_PSK with the WPA2 password you want the lab AP to use.
# `.env.test` auto-sources it. The file is gitignored.

# Start (or restart) the AP on nav-server (PSK is a CLI arg so it
# survives sudo without an env-preserving sudoers rule):
set -a; source .env.test; set +a
ssh nav-server "sudo bash /usr/local/sbin/yeydisp-lab-ap-setup.sh '$YEY_NET_PSK'"

# Start SK + simulator:
make demo-up-remote

# Provision a fresh device over BLE (only needed once — reads
# YEY_NET_PSK from env automatically):
make provision

# Tests:
make sys-test-remote

# Tear down:
make demo-down-remote
# (the AP keeps running — it's a systemd unit on nav-server.
#  to stop it explicitly: ssh nav-server 'sudo systemctl stop yeydisp-lab-ap')
```

## Files in this repo that own pieces of the rig

| File                                         | Owns                                  |
|----------------------------------------------|---------------------------------------|
| `.env.test`                                  | Env vars the test suite sources       |
| `signalk/scripts/lab-ap-setup.sh`            | hostapd + dnsmasq + iptables on nav-server |
| `signalk/scripts/run-remote.sh`              | SK container in host-network mode      |
| `signalk/scripts/stop-remote.sh`             | Stops the remote SK + local simulator |
| `signalk/config/`                            | SK home dir (rsync'd to nav-server)   |
| `signalk/plugins/signalk-espdisp-manager/`   | Local plugin source                   |
| `tests/system/`                              | pytest system suite                   |

## Why Mac runs BLE tests, nav-server runs HTTP tests

The current `nav-server` has a Realtek combo WiFi+BT chip (BT MAC 4C:49:6C:80:CB:**49**,
wlan-ap0 MAC 4C:49:6C:80:CB:**46** — same OUI prefix, shared radio).  While
hostapd hammers 2.4 GHz for `yey-net`, BlueZ's LE scan parameter setup
returns `Input/output error` and `bleak` sees only anonymous advertisements
(no names, no service UUIDs).  Coexistence is upstream-broken on this
class of chip; we don't fight it.

So the test transports split:
- **BLE NUS injection (tap/swipe/gesture)** runs from the dev Mac.
- **HTTP `/api/*`** runs through an SSH `-L 10067:10.42.0.67:80` tunnel
  through nav-server, if the WAN router doesn't yet have the static route
  to `10.42.0.0/24`.

`make sys-test-mac` opens the tunnel, sources `.env.test`, points the
suite at `localhost:10067` + `YEYBOATS_BLE_NAME=espdisp`, runs pytest,
and tears the tunnel down on exit.

## Recovery cheats

- **Device stops responding on IP but BLE works.** It's likely
  deassociated from the AP (often after a `lab-ap-setup.sh` rerun).
  Re-save credentials via BLE — that triggers reboot + fresh assoc:
  `wifi yey-net "$YEY_NET_PSK"` (from your `.env.test.local`).
- **`make demo-up-remote` chmod errors on `node_modules`.** Harmless;
  those files are owned by container uid 1000 and already have the
  right perms. The script ignores the failure.
- **`pytest` complains UDP 34301 is in use.** A stale local
  `signalk-server` Docker container from a previous `make demo-up`
  is holding the port; `make demo-down` it.
- **Plugin `signalk-autopilot` fails to start.** Known upstream
  incompat (ESM default export vs. SK 2.13's CJS loader); the plugin
  is disabled in `plugin-config-data/autopilot.json` until SK upstream
  fixes it. `test_autopilot.py` skips cleanly.
