# Stability evidence capture — 2026-06-12

First act of SP1 (evidence-first): capture live failure signatures before
changing any firmware. Captured from this workstation (192.168.2.x) and via
the lab SignalK host `mythra-nav` (SignalK 2.27.0).

## What was reachable

| Target | Result |
|---|---|
| `espdisp.local` (mDNS, from workstation) | `cannot resolve … Unknown host` |
| USB serial (`/dev/cu.usbserial-*`) | none attached |
| `mythra-nav:3000/signalk` | healthy, v2.27.0 |
| SignalK login `admin/admin` | token issued (demo creds still in place) |
| manager `GET /plugins/espdisp-manager/devices` | returns registry (below) |
| manager proxy `GET /devices/espdisp/live/status` (server→device) | `device_unreachable: device request timeout` |
| manager proxy `GET /devices/espdisp/live/logs` | `device_unreachable: device request timeout` |

## Manager registry record for `espdisp`

```
lastSeen:            2026-05-28T16:01:37Z   (~15 days stale)
firmware.version:    0.0.0-dev
firmware.build_time: May 28 2026 16:49:42
config:              v5 / hash 362ae2134b9ce51e
display:             480x480, safeArea y=62 h=418
networkIdentity:
  mdnsEnabled:           false
  currentFqdn:           espdisp.local
  lastResolvedAddress:   10.75.205.170      (from May-28 lease)
status.network:        wifi_up, sta, ip 10.75.205.170, rssi -26
status.ui.uptime_ms:   32232   (~32 s uptime at last heartbeat)
```

## Findings

1. **Device HTTP is currently dead from every angle** — not just a stalled
   UI. The server, which sits on the device's own subnet, times out hitting
   it. Either powered off, network-wedged, or (most likely) its IP changed.

2. **Discovery defect (new, reproducible from here):** mDNS is disabled on
   the device and the manager only has a **stale cached IP** from the May-28
   DHCP lease. When DHCP rotates the address, the manager has no
   re-resolution path and loses the device permanently. This is independent
   of the reboot/stall bugs and is a clean SP1 target.

3. **Manager heartbeat stopped 2026-05-28** while firmware has since shipped
   to v0.3.5. Current firmware either has the manager client disabled or
   isn't pointed at this manager — the registry has been blind for two weeks.
   The "device management" feedback loop is effectively not running.

4. **`uptime_ms ≈ 32 s` at last heartbeat** — the last successful contact was
   right after a boot, consistent with a reboot just before May-28 16:01.

5. **Lab hygiene:** SignalK still accepts `admin/admin`. Fine for lab, must
   not reach production.

## Implications for SP1

- The **soak rig must run on `mythra-nav`** (same subnet as the device), not
  this workstation — the workstation cannot reach the device directly
  (routed, ~86 ms, different subnet, no cross-subnet mDNS).
- Add **manager-side re-resolution**: on `device_unreachable`, the manager
  should re-resolve via mDNS and/or a discovery sweep rather than trusting
  the cached IP. Pair with re-enabling mDNS on the device (or a periodic
  manager-directed announce).
- Restore the **heartbeat feedback loop** so the registry reflects current
  firmware — otherwise every future stall is invisible from the server.
- First on-device step once physically reachable: pull `/api/diag`, the
  prevboot RTC ring, and BLE logs to classify the reboot signature.

## Workstream A verification (2026-06-12, same day)

Manager re-resolution fix (`fix(manager): re-resolve device address across
candidates`, commit `fc591a9`) deployed to the lab plugin install
(`/home/compulab/espdisp-signalk/plugins/signalk-espdisp-manager`, volume-mounted
into the `signalk` Docker container on mythra-nav) and the container restarted.

**Result — fall-through proven by timing.** A `live/status` proxy call to the
(still-offline) device took **9.2 s**. The pre-fix code, with a single
candidate and a 3 s HTTP timeout, returns in ~3 s. 9.2 s == three candidates
attempted sequentially (`10.75.205.170` → `espdisp.local` →
`espdisp-device.local`, each ~3 s), confirming the manager now falls through
to the mDNS FQDN instead of dying on the stale cached IP.

The device remains physically unreachable (powered off / off-network) — a
separate matter for the firmware workstreams — but the manager-side orphaning
defect is fixed: the device will be found again on any of its known addresses
the moment it returns. Host tests (`test/device-resolution.test.js`, 5/5) and
the full plugin suite pass. A pre-deploy backup of the old plugin file is at
`…/lib/manager.js.bak-2026-06-12` on the host.

## Root-cause fix: internal-heap starvation (2026-06-12, same day)

The user brought the device back online (USB-reflashed) reporting it was
"unstable for network but live." Reached it via the mythra-nav relay
(`--remote compulab@mythra-nav`, device DHCP'd to `10.42.0.67`; its IP had
indeed rotated off the May-28 lease — exactly the Workstream-A scenario).

**Captured signature.** Live `/api/state` showed internal heap
`heap_internal_free ≈ 11.5 KB`, `heap_internal_largest ≈ 7.6 KB`, while PSRAM
had 7.8 MB free. A soak run recorded the instability directly: **20/20 ticks
HTTP-unreachable** over 5 minutes (no ping reply, `http_code=000`) while the
already-open SignalK WebSocket stayed `live` — the textbook
internal-SRAM-starvation symptom (lwIP can't allocate a TX buffer / new socket,
but established connections persist).

**Root cause.** `main.cpp` allocated the two LVGL partial draw buffers
(`LCD_W*40*2 ≈ 37.5 KB` each, ~75 KB total) from
`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`, with PSRAM only as a *failure*
fallback. At boot the internal alloc succeeds, so ~75 KB of the ~90 KB internal
pool was gone before WiFi/lwIP ran. `disp_flush_cb` blits via the CPU
(`gfx->draw16bitRGBBitmap`), so the buffers need not be DMA/internal.

**Fix** (commit `46e22e3`): allocate the draw buffers from PSRAM (internal DMA
as fallback); also corrected `heap_min_ever` to the internal-only watermark
(it had reported the useless ~7.8 M PSRAM-inclusive figure).

**Measured before → after** on the bench device:

| metric | before | after |
|---|---|---|
| `heap_internal_free` | 11.5 KB | 89.5 KB |
| `heap_internal_largest` block | 7.6 KB | 77.8 KB |
| `heap_min_ever` (internal, truthful) | n/a (was bogus 7.8 M) | 79.6 KB |
| soak reachability | 20/20 ticks UNREACHABLE | **25/25 reachable** |
| soak verdict | FAIL (0 usable) | **PASS** (0 reboots, 0 stalls, min heap 81 KiB) |

Render perf held: `bench` reports flush avg 744 µs / peak 951 µs (sub-ms; the
~2 Hz figure is idle dirty-region redraw on a static MFD, not a regression).

**Remaining (not blocking):** a full 24 h soak should still run to satisfy the
SP1 done-gate; the ~92 KB of core-0 task stacks and NimBLE-resident internal
RAM remain available headroom for future tuning (BLE must stay — it is a
required diagnostic/config channel). Stack canaries (B1) should land next via
`build_flags -fstack-protector-strong` (the prebuilt Arduino core makes the
sdkconfig toggle ineffective).

## Manager loop restored (2026-06-13)

The "heartbeat stopped 2026-05-28" finding above is resolved. Root cause: the
device lost its NVS manager token (erase/reflash), and `/devices/register`
only returns `deviceToken` on first-create, so the device register-looped
(200, `auth=unprov`, no heartbeat) and the registry `lastSeen` stayed stale.

Fixed operationally by setting the shared dev token on the device
(`manager-token espdisp-dev`; lab plugin is dev-shared-token mode, default
devToken `espdisp-dev`). Result: `auth=provisioned`, heartbeat code 200, and
the registry now shows `espdisp-28372f8a0290` with `lastSeen` = now and fw
`0.3.5`. The manager feedback loop (and the per-device "switch view + reload"
path) are live again; SP4 push-live is unblocked. Proper code fix tracked in
the lab-manager-provisioning note.
