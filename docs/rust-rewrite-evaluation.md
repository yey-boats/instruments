# Evaluation: rewriting the firmware in Rust

Honest assessment as of 2026-06-03. Scope: this specific firmware on this
specific hardware (Sunton ESP32-4848S040 + custom marine MFD application),
not C++ vs Rust as a general question.

## Current footprint

| Category | LOC | Replaceability in Rust |
|---|---|---|
| Pure logic (host-testable, no Arduino) | 1 943 | Trivial; port one-for-one |
| Arduino-coupled (HTTP, BLE, OTA, WS, mDNS, WiFi) | 5 511 | Hard; ecosystem gaps |
| LVGL UI + display/touch drivers | 7 777 | Mixed; LVGL stays C either way |
| **Total** | **~19 800** | |

Six external libraries:
- `Arduino_GFX` (RGB panel + GT911) — board-specific, custom
- `LVGL 9.2` — pure C library, callable from Rust via FFI
- `gt911-arduino` (TAMCTec fork) — patched for our panel
- `NimBLE-Arduino 1.4` — BLE server (NUS service)
- `links2004/WebSockets` — SignalK WS client
- `ArduinoJson 7.2` — JSON handling

## Recent bugs — would Rust have caught them?

Bugs fixed in the last ~10 commits, classified by what Rust's compile-time
checks would have done:

| Bug | Mechanism | Rust would have… |
|---|---|---|
| `handle_logs` 19 KiB stack overflow | `LogEntry entries[96]` on 8 KiB stack | …**still missed it**. Rust enforces stack sizes at *task creation*, not per-frame. Same overflow. Stack canaries (we just enabled) catch it in both languages. |
| Manager `String resp = http.getString()` on internal heap | unbounded allocation on small heap | …**not directly caught**. Rust's `Vec::with_capacity` is the same pattern. The "use PSRAM not internal heap" decision is runtime, not type-level. |
| OTA on Arduino main loop starving | `ArduinoOTA.handle()` in `loop()` | …**not caught**. Both languages let you write a polling loop on the wrong task. |
| `[prevboot]` ring re-entrant write | recursive logf during dump | …**caught with discipline, not types**. A `Mutex<bool>` re-entrant guard reads the same in both. |
| WS heartbeat 3s pong window too tight | wrong configuration constant | …**not caught**. Configuration tuning isn't a type error. |
| `/api/state` doc on internal heap | default allocator | …**caught with an allocator-aware design**, which exists in unstable Rust (`allocator_api`) but isn't stable. Practically: same as C++. |
| `espdisp watch` reboot false-positive | None coerced to {} | …**caught**. `Option<T>` forces explicit handling. Real win — but this was a tool bug, not firmware. |

Net: of the firmware bugs from this session, **one** (`crash_ring_append`
recursion) is the kind Rust's borrow checker prevents structurally. The
rest are configuration, allocator choice, or task scheduling — all
runtime decisions Rust expresses the same as C++ does.

## What Rust would buy here

1. **Tooling**: `cargo` >>> PlatformIO + Makefile + 3 different versioning
   scripts. Single source of truth for builds, deps, tests.

2. **Sum types**: `enum` with data replaces the C++ idiom of "tagged
   struct with a union or a parallel discriminator". The
   `manager::HealthState` switch in `web.cpp` and the `WStype_*` switch
   in `signalk.cpp` become exhaustive matches the compiler enforces.

3. **`Result<T, E>` instead of negative magic numbers**: `do_heartbeat`
   returning `-1 = CONNECTION_REFUSED`, `-2 = NOT_CONNECTED`, `-5 =
   LOW_HEAP` is exactly the pattern `Result<Code, ManagerError>`
   replaces and the compiler enforces handling. This **would** have
   caught the "I thought -1 meant WiFi-down but it actually means
   connection-refused" bug I made today.

4. **No `#if ESPDISP_ENABLE_X` flag mess**: `#[cfg(feature = "demo")]`
   is structurally the same but cleaner; `cargo build --no-default-features`
   gives release/debug variants without our current platformio.ini
   release_common gymnastics.

5. **First-class async**: `embassy` on ESP32-S3 would replace our hand-
   rolled FreeRTOS task layout. Cleaner WiFi-event handling, cleaner
   `app_events` queue, cleaner SK reconnect logic. **But**: this is
   a paradigm shift, not a port.

6. **Better unit-test ergonomics**: 191 tests we already have would
   migrate cleanly. `cargo test` is faster than `pio test -e native`.

## What Rust would NOT buy here

1. **Hardware drivers**: the GT911 big-endian quirk + ST7701 RGB pin
   order + camera-verified pin map are firmware traps that survive
   the language change unchanged. We re-discover them or we port them.

2. **lwIP issues**: the TCP MSL=60s / MAX_SOCKETS=10 (F4) wedge is
   IDF-built lwIP. Same library, same behavior. Rust on ESP32 uses
   ESP-IDF underneath; we're not escaping that.

3. **LVGL is C either way**: the `lvgl-rs` crate is FFI bindings.
   Our 7 777 LOC of UI code still has to know that `lv_obj_*` calls
   are unsafe (multi-threaded LVGL is a documented memory trap in
   THIS firmware — `lvgl only on UI task` is the rule). Rust marks
   them `unsafe`, doesn't make them safe.

4. **The PSRAM allocator pattern**: stable Rust's allocator API isn't
   stable. The closest production-ready approach is a global allocator
   override, which is *less* flexible than our per-document
   `JsonDocument(&psram_json)`. We'd write a custom global allocator
   that routes >256B allocations to PSRAM, but it'd be all-or-nothing.

5. **NimBLE in Rust on ESP32**: `esp-rs/esp-hal` BLE support is `bleps`,
   far less mature than NimBLE-Arduino. Custom GATT server work would
   be substantial.

6. **ArduinoOTA equivalent**: doesn't exist in the Rust ecosystem.
   Either we use `esp_ota` raw HAL (we then build the protocol
   ourselves) or we use `esp-idf-svc` and pull in IDF anyway.

## Ecosystem maturity check (2026-06)

| Crate | Status | Risk |
|---|---|---|
| `esp-hal` (esp-rs/esp-hal) | Active, async, stable for many peripherals | Low for GPIO/SPI/I2C |
| `esp-wifi` | Beta-ish; WiFi works, BLE patchy | Medium |
| `embassy` async runtime | Production-ready on ESP32 | Low |
| `lvgl` crate | FFI bindings to LVGL C; UI code is still messy | Medium |
| `tungstenite` / `embedded-websocket` | Works; WS client OK | Low |
| `serde_json` / `serde-json-core` | Excellent | Low |
| ESP32-S3 ST7701 RGB panel driver | None I can find; would need to write | **High** |
| GT911 driver | `gt911` crate exists, untested on this panel | Medium |
| ArduinoOTA-equivalent | None; build on `esp_ota` raw API | **High** |
| NimBLE-equivalent (GATT server) | `bleps` only, limited | **High** |

The three "High" items are the path that would block delivery for weeks.

## Effort estimate

For a single developer who already knows Rust and ESP32:

| Stage | Estimate |
|---|---|
| Project skeleton (esp-hal, embassy, LVGL FFI) | 1 week |
| Pure logic port (1 943 LOC; mostly direct) | 1 week |
| Display + touch drivers from scratch | 2-3 weeks |
| WiFi + HTTP server (probably `picoserve`) | 1 week |
| BLE GATT server (`bleps`) | 2-3 weeks |
| SignalK WS client | 1 week |
| LVGL UI port (7 777 LOC) | 2-3 weeks |
| OTA implementation on `esp_ota` | 1 week |
| Testing/debug on real hardware to parity | 3-4 weeks |
| **Total** | **3-4 months** sustained |

For a developer learning Rust: add 1-2 months and significant risk that
the ESP-IDF underneath bites them anyway.

## Verdict

**Not worth it for this device.** The firmware is now stable, the bugs
are well-understood and documented in CLAUDE.md's memory traps, and the
ecosystem gaps (ST7701 RGB driver, ArduinoOTA equivalent, NimBLE server)
mean weeks of work for things that already work. The bug class Rust
catches structurally — the recursive `crash_ring_append` — is one of
seven recent bugs, and we caught it with a quick guard variable.

**Worth considering for next-generation hardware.** If a v2 device with
different panel/SoC is on the roadmap, starting that in Rust is
defensible. The pure-logic modules (`signalk_parser`, `layout`,
`autopilot_pure`, `manager_config`) port cleanly. The UI and BLE
glue cost roughly the same in either language because LVGL and NimBLE
are C either way.

**Incremental improvements available without rewriting:**

1. Use `[[nodiscard]]` aggressively on functions returning error codes
   to force handling (catches the `-1` confusion class).
2. Replace error-code negative numbers with `enum class` returns +
   `std::expected`-like pattern (C++23 has `std::expected`; we're on
   C++17 but `tl::expected` exists).
3. Already done: PSRAM allocator pattern, stack canaries on debug,
   feature flags for debug/release split.
4. Move more logic into the host-testable `_pure.cpp` modules. We
   currently have 1 943 LOC of pure logic; another ~1 000 LOC of
   business logic in `manager.cpp` and `signalk.cpp` could move out
   cleanly. That's where the most-leveraged tests live.

The single most valuable change that's *not* a rewrite: replace negative
error magic numbers in `manager.cpp` with a `ManagerStatus` enum class
and force callers to switch on it exhaustively. ~2-hour change, catches
the same class of bug I made today, doesn't require learning a new
language or shipping months of work.
