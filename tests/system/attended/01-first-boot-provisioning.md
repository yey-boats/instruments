# 01 First-boot provisioning

**Goal**: a freshly erased device can join a WiFi network through the
captive portal OR through BLE.

## Prereqs

- USB cable connected (for log monitoring + recovery)
- A phone / laptop with WiFi
- A known SSID + password to join

## Steps

### Reset WiFi state

```sh
make backup    # optional: snapshot flash before destructive ops
esptool.py erase_region 0x9000 0x5000   # wipes NVS
make flash
make monitor
```

You should see `[wifi] all saved networks failed, fallback to AP` on
serial within ~15 s of boot.

### A) Captive portal path

1. On the device screen, the **WIFI SETUP** screen appears (large QR
   code + on-screen instructions). ⬜
2. On the phone, scan the QR or join the AP (SSID printed on screen).
3. The captive portal page loads automatically. ⬜
4. Tap **Scan**; the list of nearby networks populates within 10 s. ⬜
5. Tap your target SSID, enter password, tap **Connect**.
6. The device reboots and joins. The screen returns to the dashboard
   within ~25 s; `make monitor` shows `[wifi] up: ip=...`. ⬜

### B) BLE path

After erasing NVS again, instead of using the captive portal:

```sh
make ble
```

1. The BLE console connects (look for `[ble] connected` on serial). ⬜
2. Type `wifi-list` — should print "no saved networks". ⬜
3. Type `wifi MyHomeSSID secret123` — the device saves and reboots. ⬜
4. After reboot, `ip` returns a DHCP address. ⬜

## Pass criteria

Either path A or B yields a stable STA connection that survives a
power cycle.
