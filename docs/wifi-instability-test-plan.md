# WiFi instability: alternate AP isolation test

Plan for confirming whether the chronic "ghost association" pattern
(BLE reports STA + IP + good RSSI but the AP's bridge can't ARP the
device) is caused by compulab's `wlan-ap0` interface specifically or
by the ESP32-S3 / Sunton 4848S040 itself.

## Symptom

After a fresh `wifi-reconnect` the device pings reliably. Within a
few minutes the AP-side ARP entry transitions to FAILED while the
device's BLE-reported state still shows:

```
ip=10.42.0.67  mode=STA  rssi=-49 to -53 dBm
```

`tcpdump -i wlan-ap0 ether host 28:37:2f:8a:02:90 or arp` captures
zero packets during the dead window — the device is not transmitting
anything visible to this AP.

## What this test isolates

If the device behaves the same on a different AP, the ESP32-S3 /
Sunton board is the suspect (WiFi+BLE coex, RF, driver). If it stays
stable on another AP, compulab's hostapd / dnsmasq / driver / bridge
is the suspect.

## Run

1. Use a phone hotspot or a different home AP that you can monitor.
2. Save the alternate network on the device:

```sh
# via BLE or USB serial
python3 tools/yeydisp.py ble cmd "wifi <ssid> <pass>"
# (device reboots, tries the new network first)
```

3. Watch the device for ≥1 hour. From the alternate AP host:

```sh
# every 30 s, log heap + reachability
while true; do
  T=$(date +%H:%M:%S)
  P=$(ping -c 1 -W 1 <device-ip> 2>/dev/null | grep -o "time=.*" || echo UNREACHABLE)
  echo "$T  $P"
  sleep 30
done | tee /tmp/alt_ap_watch.log
```

4. If you can also tcpdump on the alternate AP, mirror the lab capture:

```sh
sudo tcpdump -i <iface> -n -w /tmp/alt_ap.pcap \
    'ether host 28:37:2f:8a:02:90 or arp' &
```

5. After ≥1 hour, look at the watch log. Compare patterns:

| Pattern | Interpretation |
|---|---|
| Same intermittent UNREACHABLE windows | Device-side issue (ESP32 / Sunton) |
| Reliably reachable throughout | compulab AP-side issue |
| Different pattern (e.g. better RSSI) | Need to control for RF environment |

## Restore lab network

```sh
python3 tools/yeydisp.py ble cmd "wifi esp-lab <password>"
```

(or just `wifi-forget` and pick from saved networks on the next boot)
