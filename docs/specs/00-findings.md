# Architecture Findings

This document records the current implementation status against
`ARCHITECTURE_REFACTOR_PLAN.md`.

## Summary

The responsiveness refactor is on track. The important architectural direction
is now visible in the codebase:

- `app_events` provides UI and network queues.
- Web and BLE handlers mostly post commands instead of mutating LVGL directly.
- WiFi startup moved to an async manager task.
- Autopilot Signal K PUTs are posted to the network worker.
- Detached LVGL screens and PSRAM-backed LVGL allocation are in place.
- Runtime diagnostics now expose queue depth, loop timing, heap, PSRAM, WiFi,
  and Signal K state.

## Verified

- `make test` passed: 32/32 native tests.
- `make build` passed for `esp32-4848s040`.
- Firmware size after verification:
  - RAM: 18.4%
  - Flash: 26.7%

## Remaining Issues

### Layout State Is Not Fully Guarded

`layout_loader.cpp` still exposes `layout::last_json()` as a raw pointer to
`s_last_json`, while `layout::apply_json()` can free and replace that buffer.
Web and BLE read layout JSON from other tasks, so this is still a race.

Required change:

- Add a layout mutex and snapshot-copy API, for example:
  - `bool layout::copy_last_json(String &out)`
  - `bool layout::copy_summary(LayoutSummary &out)`
- Avoid returning raw shared pointers across tasks.

### Signal K Data Locking Is Partially Adopted

`sk::copyData()` exists, but most screens and web endpoints still read
`sk::data` directly. The mutex also does not currently protect connected /
disconnected state writes.

Required change:

- Convert every UI refresh and web JSON endpoint to local snapshots:

```cpp
sk::Data d;
sk::copyData(d);
```

- Protect all writes to `sk::data`, including connection status fields.

### BLE `scan` Command Still Blocks

The BLE NUS path treats `scan` as inline-safe, but `net::handleSerialCommand`
uses synchronous `WiFi.scanNetworks(false, true)`. That can block the NimBLE
callback task.

Required change:

- Remove `scan` from inline BLE commands.
- Route it through a network command or replace it with the async web/UI scan
  path.

### `/api/wifi/forget` Still Blocks In The Web Task

`/api/wifi/forget` calls `delay()` and dispatches `wifi-forget` directly.

Required change:

- Route this through the network queue, like `/api/wifi/connect`.
- Add a dedicated `ForgetWifi` command type or model it as `RunCommand` on the
  network worker.

### Header Comments Are Stale

`include/ui_screens.h` still describes pre-created children hidden with
`LV_OBJ_FLAG_HIDDEN`, but screens are now detached and swapped with
`lv_screen_load()`.

Required change:

- Update comments so future agents do not reintroduce hidden-screen rendering.

## Recommended Next Commit

Make one focused stabilization commit:

1. Add layout snapshot locking.
2. Convert all screen and web `sk::data` reads to `sk::copyData()`.
3. Queue BLE `scan`.
4. Queue `/api/wifi/forget`.
5. Fix stale comments.

