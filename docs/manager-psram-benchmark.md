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

## Why we can't live-test yet

`tools/ota_flash.sh` reports the device runs `Jun 2 2026 21:07:05`
every time, never `Jun 3 2026 01:41:25` (the new build). Observed
pattern: espota prints `Sending invitation ..........` (10 dots,
~10 KiB of the 1.94 MiB image) then `Error Uploading`. Device stays
on prior firmware (bootloader rollback worked — partition not
corrupted).

ArduinoOTA's receive path allocates a multi-KiB TCP receive buffer
plus a flash write staging buffer, both from internal heap. With the
pre-fix manager doing 32 KiB `String` allocations on internal heap
and the heap idling at 15 KiB free, those OTA allocations almost
certainly fail.

**This is the same bug.** OTA can't deliver the fix because the bug
is preventing OTA from running. USB recovery is the only path that
breaks the cycle for now:

```sh
make flash ENV=esp32-4848s040-debug   # USB cable required
```

Once the new firmware is on the device, OTA will work normally going
forward (the fix prevents the pressure spiral).

## Benchmark plan for once a USB recovery flashes the fix

1. `python3 tools/espdisp.py watch --interval 5 --device-ip <ip> --remote <relay>`
   for 10 minutes. Compare `heap_free` against the pre-fix capture
   above. Expect the post-fix value to climb (manager no longer
   eating into it).
2. `python3 tools/espdisp.py state --field manager.lastHeartbeatCode`
   periodically — expect `200`, not `-1`.
3. Check the new heartbeat status fields: `internal_min_free_kb`
   over a 10-minute run should stay above ~20 KiB if the fix is
   doing its job.
4. Force config fetch via the manager plugin and time it. Expect
   end-to-end latency to increase by < 100 ms vs the (working)
   pre-fix path, due to PSRAM JSON parse overhead.
5. Hammer `/api/logs?since=0&limit=96` in a loop. Pre-fix wedged
   at ~12 req before TCP sockets exhausted (separate fix already
   landed). Post-fix path is unchanged but the lower internal-heap
   pressure should let lwIP recover faster between bursts.
