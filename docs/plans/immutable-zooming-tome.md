# Plan: MIDL T4 — configurable spec-derived limits (9 tiles, 3×3) + steering & route screens

## Context

Two intertwined needs:
1. The firmware's per-screen tile limit is hardcoded `layout::MAX_TILES_PER_SCREEN = 4` and
   hand-mirrored into `midl::FirmwareLimits`, while the MIDL capability spec already declares
   maxTiles **per display class** (square-480=4, landscape=6). The user wants it **configurable,
   generated from the spec**, and raised to **9 (3×3)** for the 480 panel (~1″ boxes on the 4″
   screen; revisit for 3″/3.5″ later).
2. The roadmap's **steering** and **route** screens are still legacy hardcoded screens; they
   should become MIDL documents rendered by the generic renderer — the first real screens that
   exploit >4 tiles and the new gauge-range (T1) + button-action (T3) features.

PSRAM is **not** the constraint (8 MB octal; MIDL arena pool ~16 KB at 4 tiles, ~34 KB at 9 —
<0.5%). The real pressures: the per-screen `FreeformState` structs live in **scarce internal
SRAM** (~328 B/tile × ≤16 screens → ~21 KB at 4, ~48 KB at 9); the 8 KB task stack (arena stays
`memset`-in-place); the manifest contract. This plan moves `FreeformState` to PSRAM and adds
**compile-time budget asserts** so footprint is a checked `constexpr` of the configured counts.

Inventory confirmed both screens are fully expressible — every path already has a MetricSource,
the rudder ±35° gauge works via T1's `format.range`, and nudge buttons dispatch the existing
`autopilot heading <delta>` command via T3's button actions. No new painters or MetricSources.

---

## Part A — Configurable, spec-derived limits (decoupled from legacy POD)

**Do NOT bump `layout.h:15`** — `layout::Config` embeds `Screen[8] × Tile[MAX_TILES_PER_SCREEN]`;
9 tiles grows it ~2.25× and blows its ≤48 KB guard (`test_midl_projection.cpp:99`, ~44 KB now).
Decouple: legacy stays 4, MIDL gets its own spec-derived limit.

1. **Spec source (MIDL submodule — cross-repo).** `midl/cpp/include/yb_midl_catalog.h`
   `CLASSES[]`: `{"square-480",480,480,2,2,4,3}` → `{"square-480",480,480,3,3,9,3}`. Regenerate the
   JSON with `make gen-manifest` (compiles `midl/cpp/tools/gen.cpp`). Commit in the submodule +
   bump the submodule pointer in espdisp.
2. **Codegen firmware limits.** Extend `tools/embed_manifest.py` to also parse the class JSON and
   emit `include/generated_midl_limits.h`: `namespace midl_gen { constexpr int MAX_TILES=9;
   constexpr int MAX_DEPTH=3; constexpr const char *CLASS_ID="square-480"; }`. Committed artifact
   like `generated_midl_manifest.h`; same `make embed-manifest`.
3. **Consume (decoupled).** `include/midl_limits.h`: include the generated header;
   `max_tiles_per_screen = midl_gen::MAX_TILES`, `MAX_LAYOUT_DEPTH = midl_gen::MAX_DEPTH`. Keep
   `max_screens` firmware-defined. `src/ui/ui_layouts.cpp:2669`
   `FREEFORM_MAX_TILES = (int)midl::FirmwareLimits::max_tiles_per_screen`. Solver
   (`PlacementSet.items[]`) and arena (`midl_render_apply.cpp:66 MAX_TILES`) already read
   `FirmwareLimits` → grow automatically; `solve_grid` is already generic for 3×3.

## Part B — 9-tile render hardening

4. **FreeformState → PSRAM.** `create_freeform` (`ui_layouts.cpp`): change the `FreeformState`
   alloc `MALLOC_CAP_INTERNAL` → `MALLOC_CAP_SPIRAM` (moves ~48 KB off internal SRAM at 9×16;
   read at 5 Hz on the UI task — PSRAM latency fine). **Flag for HW validation** (inverse of the
   internal-starvation trap, but still a heap-placement change).
5. **Compile-time budget asserts (the "limits at compile time" ask).** Add `constexpr` budgets in
   `midl_limits.h` + `static_assert`s: in `midl_render_apply.cpp`
   `ui::MAX_SCREENS * sizeof(MidlScreenArena) <= MIDL_ARENA_PSRAM_BUDGET`; in `ui_layouts.cpp`
   `sizeof(FreeformState) <= MIDL_FREEFORM_STATE_BUDGET`. Oversized config fails the build, not
   the device.
6. **Painter tuning for ~160 px tiles.** `build_tile`/painters are sized for ~232 px (cap
   `montserrat_20`, value up to `montserrat_48`). For a true 3×3 (160 px) tile, value/cap fonts
   overflow. Add a tile-size-aware font step-down in the painters that already branch on `w`/`h`
   (`paint_numeric_body`, `paint_gauge_body`, `paint_compass_body`, `paint_trend_body`): pick a
   smaller font tier when `w < ~180`. Keep existing tiers for larger tiles (split layouts).

## Part C — Steering MIDL screen

7. Author a `steering` MIDL screen (in the baked `include/midl_demo_doc.h` for MIDL-only-boot
   validation; mirror to `midl/library/steering.midl.yaml` for the authoring catalog). Layout =
   top weighted-split holding a **2×2 grid of 4 instruments** over a bottom **row-split of 4
   nudge buttons** (8 leaves ≤ 9, depth 2 ≤ 3):
   - compass `navigation.headingTrue` + dir `…bearingTrackTrue` (HDG with CTS marker)
   - gauge `steering.rudderAngle`, `format.range:[-35,35]`, precision 0 (graphical rudder — uses T1)
   - single-value `…crossTrackError` (XTE, unit nm), single-value `…velocityMadeGood` (VMG, kn)
   - 4 buttons, `action.kind:"command"`: `"autopilot heading -10"`, `"-1"`, `"1"`, `"10"`
     (verified handler `autopilot.cpp:203`). (No `tack`/`mode` buttons — `tack` has no command
     handler; `autopilot mode <m>` exists if a mode button is wanted later.)

## Part D — Route MIDL screen

8. Author a `route` MIDL screen (same homes). Faithful port of the legacy 2×2:
   - single-value `…nextPoint.distance` (DTW, nm), single-value `…nextPoint.bearingTrue`
     (BTW, with CTS secondary), single-value `…crossTrackError` (XTE, nm),
     single-value `…velocityMadeGood` (VMG, kn).
   - Optionally enrich toward the 9-tile budget (ETA/waypoint/SOG/course-trend) — kept minimal
     unless desired.

## Part E — PathStore (clarification, not blocking)

9. The steering/route screens resolve **entirely via the MetricSource enum bridge** (all paths
   mapped in `metric_source_map.cpp`) — the generic `PathStore` is **not required** for them.
   Do the cheap defensive bit only: `include/path_store.h:19` `CAP 64 → 160` + fix the
   `maxTiles` comment (theoretical bound 8×9×2=144; Entry≈96 B → ~15 KB PSRAM). Full generic
   PathStore wiring on the MIDL path stays a genuinely separate future slice (no screen here
   needs it).

---

## Tests
- `test/test_midl_solve/`: add a 3×3 grid case (rows=3,cols=3, 9 cells) → 9 placements, correct
  row-major rects (mirror `test_grid_2x2_rowmajor`); and a steering-style split (grid-over-row)
  case asserting 8 leaves.
- `test/test_midl_projection/`: optionally assert `FirmwareLimits::max_tiles_per_screen == 9`;
  the `sizeof(layout::Config) ≤ 48 KB` guard stays green (POD untouched). Update "4tile" comments.
- Existing 350 host tests stay green.

## Verification (end-to-end)
- **Regen + host**: `make gen-manifest && python3 tools/embed_manifest.py` (both headers), then
  `pio test -e native` — all green incl. new solver tests; Config guard ≤48 KB holds.
- **Build**: `pio run -e esp32-4848s040` — confirms the `static_assert` budgets pass and the
  decoupled limits compile.
- **Device** (lab 10.42.0.67 via `compulab@mythra-nav`, MIDL-only + LAB_OPEN_WEB):
  `PLATFORMIO_BUILD_FLAGS="-D YEYBOATS_MIDL_ONLY=1 -D YEYBOATS_LAB_OPEN_WEB=1" make ota-verify DEVICE_IP=10.42.0.67 REMOTE=compulab@mythra-nav`.
  - Boot shows the baked steering + route screens; navigate (`screen steering` / `screen route`).
  - Screenshot via `/api/screenshot.png`: steering = compass + ±35° rudder gauge + XTE/VMG + 4
    nudge buttons; route = DTW/BTW/XTE/VMG. No reboot.
  - Live-push a pure 3×3 (9-tile) doc to validate the cap + painter tuning at 160 px.
  - Check `/api/state` heap fields for internal-SRAM headroom (confirms the PSRAM move helped).
- Commit: submodule bump + regenerated `generated_midl_manifest.h` + `generated_midl_limits.h` +
  firmware changes.

## Notes / risks
- **Cross-repo**: Part A edits the `midl/` submodule (catalog) → commit there + bump pointer.
- **HW-validate** the `FreeformState`→PSRAM move specifically (heap-placement change).
- Button **taps can't be actuated remotely** (HTTP touch injection is 403-blocked; BLE/serial
  only) — steering nudge buttons are validated by render + the proven `dispatchCommand` path, as
  with T3; physical actuation needs a person/BLE at the device.
- A parallel branch (`feat/idf5-double-buffer`) unified USB-serial dispatch through
  `net::dispatchCommand`; no conflict here, but worth a rebase check before the steering buttons
  land.
