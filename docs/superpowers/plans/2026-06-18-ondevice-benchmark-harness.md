# On-device benchmark harness (`bench-sweep`) — Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans / subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** A one-command, on-device benchmark that walks every non-hidden screen (~4 s dwell, first second discarded) under two render modes (typed vs. dynamic-store) and prints a per-screen table of render, CPU, throughput, network and memory metrics — the standing verification harness for the generic path store (Slice 1) and per-screen subscriptions (Slice 3).

**Architecture:** Cheap monotonic counters in the SK/parser/render paths are sampled once per second by the existing `fps_tick` (1 Hz `lv_timer`). A non-blocking sweep state machine (also ticked by `fps_tick`) advances screen→screen and mode→mode, accumulates per-second samples into a row, and emits a formatted table via `net::logf` (serial + BLE + UDP :9999). The pure accumulation/formatting logic is host-tested; counters and the state machine are device-build verified. **On-device execution requires hardware access** — this host cannot reach the device, so the run + result capture is performed by an operator at the device.

**Tech Stack:** C++17, PlatformIO, LVGL timers, FreeRTOS runtime stats (optional), ESP-IDF temp sensor, Unity (host tests).

## Locked metric set ("everything reasonable")

Per screen, per render mode:

| Group | Metrics | Source |
|---|---|---|
| Render | fps (flush/s), flush avg µs, flush peak µs, **refresh µs** (5 Hz UI refresh), lvgl pump peak µs | have + time `ui_refresh` |
| CPU | **core0 idle %**, **core1 idle %**, **per-task CPU %** (top tasks), loop peak µs | FreeRTOS runtime stats |
| Throughput | **deltas/s** (value-bearing), **values parsed/s**, **JSON parse avg/peak µs**, **store size**, **store lookups/s** | counters in `applyDelta`/store |
| Network | **ws frames/s**, **ws bytes/s**, **subscriptions** (active path count) | counters in `onText` + `subscribe()` |
| Memory | heap free, **heap low-water**, **largest free block**, psram free, **min task stack high-water** | `heap_caps_*`, `uxTaskGetStackHighWaterMark` |
| Latency | **screen-build → first-frame µs**, **screen-show → first-delta-for-screen ms** | timestamps on show/flush/delta |
| Thermal | **chip temp °C** | `temperature_sensor` (esp_temperature_sensor) |

Bold = new instrumentation. Two render modes: `typed` (current `sk::Data` reads) and `store` (resolution routed through `sk::PathStore`, the Slice-1 store), toggled by a global flag honored in `format_metric`.

## Methodology

- Iterate `ui::screen_count()`, skipping `is_hidden(i)` (wifi/settings/touch-cal/etc.).
- For each render mode ∈ {typed, store}: for each non-hidden screen: `ui::show(i)`; dwell 4 ticks; **discard tick 1** (screen-switch settle); average ticks 2–4.
- Network/throughput counters are reset+sampled each tick (per-second rates).
- After both modes, print the table and a `delta` summary (store − typed), then restore the original screen + mode.
- Counters are monotonic `volatile uint32_t`, read-and-zeroed in `fps_tick`; no locks (single-writer per counter; coarse sampling tolerates the race).

## Instrumentation map (files)

- `src/signalk.cpp` — `onText`: `g_ws_frames++`, `g_ws_bytes += len`, `g_deltas += max(n,0)`; `subscribe()`: record `g_sub_count`. JSON parse timing around `applyDelta`.
- `src/signalk_parser.cpp` — `apply_delta_impl`: count values parsed; (store lookups counted in `PathStore`).
- `include/path_store.h` / `src/path_store.cpp` — add a `lookups()` monotonic counter (incremented in `get`/`has`), `size()` already present.
- `src/ui/ui_layouts.cpp` — `format_metric`: when `g_bench_store_mode`, shadow-resolve via `sk::dynamicStore().get(source_to_path(m.source))` (incurs the real store-port cost). Add `source_to_path(MetricSource)` reverse map.
- `src/main.cpp` — time `ui_refresh`; `disp_flush_cb` first-frame timestamp; the sweep state machine + table; `bench-sweep` command in `handleMainCommand`; CPU runtime-stats + temp readers; stack high-water across known tasks.
- `include/bench_row.h` / `src/bench_row.cpp` (new, pure) — `BenchRow` accumulator (add sample, finalize average) + table row formatter. **Host-tested.**
- `platformio.ini` — if `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` is off, add `board_build.cmake_extra_args`/`sdkconfig` override, or fall back to loop-headroom %. Add `bench_row.cpp` to native filter + `test_bench_row` to `test_filter`.

## sdkconfig dependency (CPU %)

Per-core idle % and per-task CPU % need `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` + `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`. On Arduino-ESP32 these are often default-on (esp-idf 5.x); **verify** with a probe build. If unavailable, report core idle % via a loop-headroom proxy (idle-delay time / period) and omit per-task %.

## Tasks (summary; expand each into TDD steps at execution time)

1. **Pure `BenchRow`** (host-tested): accumulate per-second samples for all numeric metrics, finalize to averages/peaks, format a fixed-width table row + header. `test_bench_row` Unity tests.
2. **Network + throughput counters**: `signalk.cpp`/`signalk_parser.cpp`/`path_store.cpp` monotonic counters + getters; JSON parse timing. Device build.
3. **Render-mode shadow + `source_to_path`**: `g_bench_store_mode` + reverse map + `format_metric` hook. Device build.
4. **CPU / memory / thermal readers**: runtime-stats core idle % (+ proxy fallback), stack high-water, chip temp; probe sdkconfig. Device build.
5. **Latency hooks**: screen-build→first-frame, show→first-delta. Device build.
6. **Sweep state machine + `bench-sweep` command + table**: drive modes×screens via `fps_tick`; emit table + delta; restore. Device build.
7. **Operator run**: flash/OTA; `bench-sweep`; capture table from serial/BLE/live-logs; paste into results doc.

## Risks

- **Unverifiable here:** this host can't reach the device; an operator runs Tasks 7. All firmware tasks are device-build verified + host-tested where pure, but on-device behavior (timer state machine, restore) needs an operator smoke test.
- **Counter races:** coarse 1 Hz sampling tolerates unlocked monotonic counters; do not derive correctness from them.
- **sdkconfig:** CPU % may require a config change; proxy fallback keeps the harness working.
- **No new always-on cost:** all counters are increments; the sweep + shadow run only while `bench-sweep` is active, so normal operation is unaffected.
