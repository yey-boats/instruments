# Lab topology

This is the layout the firmware is tested against day-to-day.  Two
networks, one router on `cynas`, one router-and-AP on `compulab`.

```
       cynas LAN — 192.168.2.0/24
       (boat WiFi / wired)
                |
                |  static route on cynas:
                |    10.42.0.0/24 via 192.168.2.11
                |
   +------------+------------+
   | cynas router  192.168.2.1                       |
   | dev workstation  192.168.2.3 (Mac)              |
   | compulab  192.168.2.11  ← eth0, default gateway |
   +-------------------------------------------------+
                |
                |  eth0  192.168.2.11/24
                |        (SK in Docker `--network host`, exposes 3000/10110/34300/34301)
                |
        +-------+-------+
        |    compulab   |     (also runs the lab AP)
        +-------+-------+
                |
                |  wlan-ap0  10.42.0.1/24   (gateway for the AP segment)
                |  hostapd on phy0, SSID `esp-lab`, 2.4 GHz ch 6, WPA2-PSK
                |  dnsmasq DHCP 10.42.0.50-150 (static lease for the MFD)
                v
        esp-lab AP — 10.42.0.0/24
                |
                |  routed (no NAT) — IP forwarding on,
                |  iptables FORWARD ACCEPT both ways
                v
        +---------------+
        |  ESP32-S3 MFD |   10.42.0.67   28:37:2f:8a:02:90
        |  (espdisp)    |   gw 10.42.0.1, dns 192.168.2.1 + 8.8.8.8
        +---------------+
```

## Why routed instead of bridged or NAT

- **Bridged into cynas** would be the simplest UX (mDNS works, single
  broadcast domain) but it ties `eth0` into a bridge, and any glitch
  there drops SSH to `compulab` — recovery requires physical access.
- **NAT** is what `nmcli`'s `ipv4.method=shared` gives you out of the
  box, but then nothing on the cynas LAN can reach an AP-side device by
  IP, which kills OTA-from-laptop.
- **Routed** keeps `compulab` SSH stable (eth0 untouched), gives full
  L3 reachability in both directions once cynas knows where the
  segment lives, and lets `tcpdump` on `wlan-ap0` see raw device
  traffic.

## What lives where

| Service             | Host         | Listens on                 |
|---------------------|--------------|----------------------------|
| SignalK server      | `compulab`   | `0.0.0.0:3000` (host net)  |
| NMEA 0183 TCP       | `compulab`   | `0.0.0.0:10110`            |
| SK discovery UDP    | `compulab`   | `0.0.0.0:34300/udp`        |
| device-announce UDP | `compulab`   | `0.0.0.0:34301/udp`        |
| `espdisp-manager`   | `compulab`   | `/plugins/espdisp-manager` |
| `fake_boat.py`      | dev laptop   | pushes to SK over WS       |
| `hostapd`           | `compulab`   | `wlan-ap0`                 |
| `dnsmasq` (DHCP)    | `compulab`   | `wlan-ap0`                 |

## What the laptop / cynas side needs

The cynas router needs **one** static route so OTA, `pytest`, and ad-hoc
`curl` from anywhere on `192.168.2.0/24` reach the MFD:

```
Destination: 10.42.0.0
Netmask:     255.255.255.0
Gateway:     192.168.2.11
```

If you can't (or don't want to) touch the cynas router, a per-machine
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
# ESP_LAB_PSK with the WPA2 password you want the lab AP to use.
# `.env.test` auto-sources it. The file is gitignored.

# Start (or restart) the AP on compulab (PSK is a CLI arg so it
# survives sudo without an env-preserving sudoers rule):
set -a; source .env.test; set +a
ssh compulab@192.168.2.11 "sudo bash /usr/local/sbin/espdisp-lab-ap-setup.sh '$ESP_LAB_PSK'"

# Start SK + fake_boat:
make demo-up-remote

# Provision a fresh device over BLE (only needed once — reads
# ESP_LAB_PSK from env automatically):
make provision

# Tests:
make sys-test-remote

# Tear down:
make demo-down-remote
# (the AP keeps running — it's a systemd unit on compulab.
#  to stop it explicitly: ssh compulab 'sudo systemctl stop espdisp-lab-ap')
```

## Files in this repo that own pieces of the rig

| File                                         | Owns                                  |
|----------------------------------------------|---------------------------------------|
| `.env.test`                                  | Env vars the test suite sources       |
| `signalk/scripts/lab-ap-setup.sh`            | hostapd + dnsmasq + iptables on compulab |
| `signalk/scripts/run-remote.sh`              | SK container in host-network mode      |
| `signalk/scripts/stop-remote.sh`             | Stops the remote SK + local fake_boat |
| `signalk/config/`                            | SK home dir (rsync'd to compulab)     |
| `signalk/plugins/signalk-espdisp-manager/`   | Local plugin source                   |
| `tests/system/`                              | pytest system suite                   |

## Recovery cheats

- **Device stops responding on IP but BLE works.** It's likely
  deassociated from the AP (often after a `lab-ap-setup.sh` rerun).
  Re-save credentials via BLE — that triggers reboot + fresh assoc:
  `wifi esp-lab "$ESP_LAB_PSK"` (from your `.env.test.local`).
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
