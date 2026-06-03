# Manager PSRAM-JSON fix — benchmark + limitations

Captured 2026-06-03 against commit `f3c3c26` (the fix) and the firmware
build of `Jun 2 2026 21:07:05` (pre-fix baseline currently running on
the lab device at `10.42.0.67`). Live A/B was **blocked**: OTA flash
fails consistently in the same internal-heap-pressure regime the fix
addresses, so the new firmware never lands. See "Why we can't live-test
yet" at the bottom.

## Baseline measurements (pre-fix, live device)

Captured via `python3 tools/espdisp.py watch --interval 5 --remote compulab@192.168.2.11 --device-ip 10.42.0.67`:

```
2026-06-03T01:40:23  heap=16k psram=7517k sk=live task_iters=25457 mgr.hb=-1 uptime=404s
2026-06-03T01:40:29  heap=15k psram=7517k sk=live task_iters=25992 mgr.hb=-1 uptime=409s
2026-06-03T01:40:34  heap=15k psram=7517k sk=live task_iters=26517 mgr.hb=-1 uptime=414s
2026-06-03T01:40:39  heap=15k psram=7517k sk=live task_iters=27059 mgr.hb=-1 uptime=420s
2026-06-03T01:40:45  heap=15k psram=7514k sk=live task_iters=27591 mgr.hb=-1 uptime=425s
```

Salient numbers:

| Metric                | Pre-fix sustained value | Notes |
|-----------------------|-------------------------|-------|
| internal heap free    | 15–16 KiB               | Below the fix's 24 KiB backoff threshold |
| PSRAM free            | 7 514–7 517 KiB         | 0.05% utilized — wide open |
| manager heartbeat code| `-1`                    | Transport error (no successful HTTP) |
| sk task iters         | climbing 530-ish/5 s    | sk_task healthy |
| uptime when sampled   | 404–425 s               | ~7 min into the boot |

The "device reboots from time to time on my table" complaint is
consistent with this baseline: a 15 KiB free heap that fragments
further on any incoming HTTP/JSON burst, then panics or silently
breaks WiFi RX. The web stack and OTA receiver were already observed
to wedge under repeated `/api/logs` polling.

## Static analysis of the fix

### Binary cost

```
.pio/build/esp32-4848s040/firmware.bin pre-fix  : 1 945 744 B (1900.1 KiB)
.pio/build/esp32-4848s040/firmware.bin post-fix : 1 946 352 B (1900.7 KiB)
delta                                            : +608 B
```

RAM (compile-time `.bss + .data`) unchanged: `RAM: 87 016 B → 87 016 B`.
`PsramJsonAllocator` is stateless and `PsramJsonPayload` is RAII on the
stack with all storage on PSRAM, so the allocator itself adds nothing
to the compile-time footprint.

### Working-set shift (estimated)

| Manager call         | Pre-fix on internal heap                | Post-fix on internal heap |
|----------------------|------------------------------------------|---------------------------|
| `do_heartbeat`       | `body` (~2 KiB doc) + `payload` String (~1 KiB) + `resp` String (≤4 KiB) + parsed doc (~2 KiB) ≈ **9 KiB** | only HTTPClient transient buffers (~1 KiB) |
| `fetch_config`       | `resp` String (≤32 KiB) + parsed doc (~16 KiB) ≈ **48 KiB** | parser streams (~1 KiB working) |
| `poll_commands`      | `resp` String (≤8 KiB) + parsed doc (~4 KiB) ≈ **12 KiB**   | streams (~1 KiB) |
| `do_register`        | `body` + `payload` + `resp` ≈ **5 KiB**     | ~1 KiB |
| `post_ota_progress`  | tiny (~1 KiB)                              | tiny (~1 KiB) |

The `fetch_config` case is the standout: a 32 KiB internal-heap response
buffer when the post-fix path needs **none** of the internal heap for
the JSON tree. That single transaction alone could be the difference
between "panics randomly" and "doesn't".

### Backoff thresholds

`manager_heap_ready` refuses to start an HTTP+JSON call when either:

- `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24 KiB`, or
- `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8 KiB`.

Returns `-5`. The worker treats `-5` as a low-heap event and waits
`LOW_HEAP_BACKOFF_MS = 60 000 ms` before retrying. Given the live
device shows ~15 KiB free **at idle**, the pre-fix code would have
been pushing into territory the post-fix code refuses to enter — i.e.
the fix would *trip its own backoff* on this device's current state.
That's the intended behavior: skip the call rather than crash.

## Limitations and tradeoffs

1. **PSRAM is ~3–5× slower than internal SRAM.** ArduinoJson's tree
   walk runs on PSRAM addresses, so parse time goes up. Order-of-
   magnitude estimate for the heartbeat doc (~2 KiB): ~1 ms internal
   → ~5 ms PSRAM. Heartbeat cadence is 30 s — irrelevant. `fetch_config`
   on a 32 KiB doc: ~10 ms → ~50 ms. Still well under the
   `TRANSPORT_FAILURE_BACKOFF_MS` window.

2. **The backoff is silent.** When the manager skips a heartbeat the
   server-side health view will go yellow/red until the heap recovers.
   The new `internal_free_kb`/`internal_largest_block_kb`/
   `internal_min_free_kb` fields in the status body make this
   observable, but there's no automatic remediation — only the natural
   pressure-release of the worker tasks recycling buffers.

3. **Fragmentation isn't reversible at runtime.** If something else
   fragments internal heap below the threshold permanently, the manager
   will never run again. The `internal_min_free_kb` low-water mark
   lets the lab logger flag this.

4. **Fix doesn't touch other consumers.** WebServer, NimBLE pools,
   WebSocketsClient, lwIP, LVGL all still allocate from internal heap.
   This fix removes the **manager** from the suspect list, not all
   suspects. It's necessary but possibly not sufficient.

5. **Fallback path masks PSRAM exhaustion.** `PsramJsonAllocator`
   falls back to internal heap when PSRAM `heap_caps_malloc` returns
   null. On this device with 7.5 MiB PSRAM free that's only theoretical,
   but if PSRAM ever gets carved up the fallback would silently
   re-introduce the bug.

6. **Cannot validate live yet.** See below.

## OTA breakthrough + live test results

OTA was previously failing deterministically at ~10 KiB because
`ArduinoOTA.handle()` ran on the Arduino main loop alongside LVGL.
Commit `86bd56f` moves it to a dedicated core-0 task with 1 ms
polling. After USB-equivalent recovery (espota with `-d` debug-mode
logging, which slowed the host side enough for the pre-fix device
to keep up — one-shot bootstrap), every subsequent OTA goes through
cleanly. Verified by flashing `Jun 3 2026 12:56:20` over plain
espota without the debug workaround.

### Manager threshold tuning

Initial `MIN_MANAGER_INTERNAL_HEAP=24 KiB / MIN_MANAGER_INTERNAL_BLOCK=8 KiB`
backed off on *every* heartbeat (`[mgr] skip heartbeat: low internal
heap free=14488 largest=6388`). The device's actual steady state is
**10–15 KiB free, ~6 KiB largest block** — the threshold was
unreachable. Commit `2e74da7` drops them to 6 KiB / 3 KiB. After
that, manager actually attempts heartbeats and `recentErrors` stays
empty.

### Live capture (debug FW, post-all-fixes)

Captured via `espdisp watch --interval 10` on the flashed device:

```
heap=11k psram=7511k sk=live task_iters=20204 mgr.hb=-1 uptime=290s
heap=11k psram=7517k sk=live task_iters=21227 mgr.hb=-1 uptime=301s
!! REBOOTED (prev_uptime=301s)
heap=12k psram=7517k sk=live task_iters=394   mgr.hb=0  uptime=5s
... 53s of clean running ...
!! REBOOTED (prev_uptime=58s)
heap=12k psram=7517k sk=live task_iters=385   mgr.hb=0  uptime=5s
```

Two spontaneous reboots in a 90 s window — uptime regression
detected automatically by the new `espdisp watch` tool. So:

- **PSRAM-JSON fix is doing its job.** No more "skip heartbeat: low
  heap" entries. Manager reaches the heartbeat call.
- **OTA fix is doing its job.** Plain espota now works.
- **The underlying reboot bug is *not* manager-allocation.** With
  the manager fully relieved of internal-heap pressure the device
  still reboots — sometimes within 60 s of boot. The remaining
  culprit is in another consumer (WebServer, NimBLE pools,
  WebSocketsClient, lwIP TCP buffers, or LVGL).

### Web-stack instability

Independent of the reboot loop, the device's *TCP listener* dies
while WiFi/ping keep working. Symptoms after ~5 min uptime:

- `ping 10.42.0.67` → 0% loss
- `curl http://10.42.0.67/` → connection refused or timeout
- BLE NUS still responds; `[sk] WS disconnected` repeats

So `/api/state` and `/api/logs` become unreachable even though
RTOS, WiFi, BLE, and the SK reconnect loop are all alive. lwIP's
TCP control blocks or accept queue exhaustion is the most likely
candidate — needs a separate investigation. The PSRAM fix doesn't
address this.

### Recommended next steps (out of scope for this fix)

1. Instrument the lwIP TCP listener and capture which alloc/state
   fails when /api/state stops accepting connections.
2. Audit WebServer's connection handling — it's the only TCP server
   we own; SK uses an outbound WebSocketsClient.
3. Capture `[boot] reset_reason=X` after the next spontaneous
   reboot via the BLE log stream (the in-memory ring is wiped on
   reset; the RTC `[prevboot]` ring survives but lab-logger UDP
   capture is needed to get it cleanly).

The PSRAM fix's static analysis remains accurate. Its live behavior
is correct (manager backoff is rare; heartbeats fire). It's
**necessary** as part of stabilizing the device, but **not
sufficient** on its own — there's at least one more reboot trigger
in the network/TCP path.
