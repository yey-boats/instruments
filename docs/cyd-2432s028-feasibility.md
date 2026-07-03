# CYD ESP32-2432S028 (2.8" 320x240 ILI9341) — feasibility note

Status: **not feasible without a major no-PSRAM firmware profile — not implemented.**
Assessed 2026-07 while adding the Sunton 8048S0x0 and Waveshare 1.28" boards
(feat/2026-07-mfd-overhaul).

## The board

The "Cheap Yellow Display" ESP32-2432S028 is a classic **ESP32** (dual-core
LX6, ~320 KB usable internal SRAM, **no PSRAM**) with a 320x240 ILI9341 over
plain SPI and XPT2046 resistive touch. Two hard mismatches with this firmware:

1. **No PSRAM at all.** Not "small PSRAM" — the chip has none and the module
   has no PSRAM die.
2. **Not an ESP32-S3.** Every firmware env is S3 (`board = esp32-s3-devkitc-1`,
   `qio_opi`, S3 ROM cache ops); CI merges images with `--chip esp32s3`.

## Hard PSRAM dependencies (grep-verified)

PSRAM is a load-bearing design decision, not an optimization:

- `src/lvgl_alloc.cpp` — **all** LVGL allocations are routed to
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with **no
  internal-RAM fallback**. Every widget, style, canvas, and screen tree lives
  in PSRAM; all 10+ screen trees are kept resident for the session.
- `src/layout_loader.cpp:90` — the live `layout::Config` (~34 KB POD) is
  `heap_caps_calloc(1, sizeof(Config), MALLOC_CAP_SPIRAM)`. CLAUDE.md documents
  that keeping it in internal SRAM starves NimBLE and hangs the controller.
- `include/psram_json.h` — shared ArduinoJson allocator used by manager/web/SK
  hot paths; the internal-heap fallback exists but the design comment states
  the S3 idles at 10–15 KiB free internal heap, i.e. the fallback is a
  limp-home, not a budget.
- `src/midl_render_apply.cpp` — the MIDL per-screen arena pool
  (`ui::MAX_SCREENS * sizeof(MidlScreenArena)`, budget-checked against
  `MIDL_ARENA_PSRAM_BUDGET`) is PSRAM-allocated.
- LVGL draw buffers (`main.cpp`): `LCD_W*40*2` bytes ×2 in
  `MALLOC_CAP_SPIRAM`; the internal-DMA fallback is explicitly described as
  the path that previously starved WiFi/lwIP.
- `ui_markers` glyph canvases, screenshot buffers, SK WebSocket JSON documents
  — 14 TUs reference `MALLOC_CAP_SPIRAM`.

On top of that, WiFi + NimBLE + WebSockets + WebServer + ArduinoOTA together
already consume most of a classic ESP32's internal heap before a single LVGL
object is allocated.

## What a no-PSRAM profile would require

Not a port, a second firmware profile:

- Replace `lvgl_alloc.cpp` with a bounded internal pool (≤ ~48 KB `LV_MEM_SIZE`)
  and make screens **build/destroy on navigation** instead of session-resident
  (the current screen manager caches every visited tree).
- An internal-RAM `layout::Config` variant with shrunk bounds
  (`MAX_SCREENS`, `MAX_TILES_PER_SCREEN`, string tables) to get ~34 KB down to
  single-digit KB — a diverging layout ABI.
- Disable MIDL rendering entirely or shrink the arena limits (regenerate
  `generated_midl_limits.h`) — the arena pool assumes PSRAM.
- Drop or time-share one of WiFi/BLE (both do not fit alongside LVGL on ~320 KB).
- New display/touch stack (ILI9341 SPI + XPT2046) — the easy part.
- A parallel `platform`/`board` line (classic ESP32, 4 MB flash, different
  partition table, no `qio_opi`), plus CI changes (`--chip esp32`).

Estimated cost is comparable to the parked IDF-5 migration (spec 21 A):
days-to-weeks, with an ongoing "two memory models" maintenance tax on every
future feature.

## Verdict

Skip. If a low-cost SPI-display target is ever wanted, prefer an **ESP32-S3**
CYD-class variant with PSRAM (e.g. Guition JC2432W328 with S3/PSRAM or the
Waveshare ESP32-S3-Touch-LCD-2.8) — those fit the existing memory model and
would only need the SPI display/touch init branch added for the Waveshare
1.28" round board.
