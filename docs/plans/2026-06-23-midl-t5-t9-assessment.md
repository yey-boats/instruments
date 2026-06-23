# MIDL T5–T9 — current-state assessment & next-session plan (2026-06-23)

> Written while completing the "scan for incomplete features" pass. The older
> task definition (`docs/superpowers/plans/2026-06-22-yb-midl-next-session-tasks.md`)
> is **stale**: T1–T4 and most of T6 have since shipped. This re-grounds T5–T9
> against the code as it actually is on `main` + `feat/complete-incomplete-features`,
> and records why none of T5–T9 was *completed* this session.

## Why nothing in T5–T9 landed this session

Two hard constraints, both real:

1. **No hardware validation.** The lab device (10.42.0.67) is down (see the
   handoff in `.remember/`), and the USB device carries no live SignalK data.
   Every remaining T5–T9 item is either a new on-screen render or a flash/boot
   behavior — neither is verifiable by host tests or a device *build*. Shipping
   it "done" would violate verification-before-completion.
2. **Cross-repo + undefined semantics.** All of T5 needs changes in the `midl/`
   git submodule (catalog + JSON schema + TS validator + web renderer), not just
   firmware. Critically, the attrs T5 would use are **not actually specified**
   yet (see below), so implementing them firmware-first would bake in semantics
   the language hasn't agreed on.

What *was* completed this session is the shared value layer that several of
these will build on: `ui::layouts::metric_value` / `metric_unit_fraction`
(`include/metric_value.h`), host-tested and now used by the MIDL renderer, the
legacy template renderer, and the manager `widget_registry`.

## Verified current state (what already works — plan doc is stale)

- **Element kinds rendering** (`enum WidgetKind`, `src/midl_render.cpp`
  `token_to_kind`): numeric/single-value, compass, gauge (range display ✓),
  bar, windrose, autopilot (state pill + nudge), text, button (nav + command
  actions ✓), trend (sparkline ✓), **windsteer** (laylines ✓). → **T1, T2, T3,
  and the bulk of T6 are DONE.**
- **Baked demo doc** (`include/midl_demo_doc.h`) has 9 screens: dash, nav,
  speed, steering, wind, wind_steer, autopilot, route, gallery. → **T4 done.**
- **Delivery**: `POST/GET /api/midl/config`, `POST /api/midl/reset`. Host-tested
  selection (`test_midl_select`). 362 host tests green.

## Remaining gaps, precisely

### T5 — extension bindings/elements (NOT started; needs `midl/` design first)
Confirmed against code:
- **No `local`/device-state binding kind.** `MetricBinding.source` is a
  `MetricSource` enum over `boat::View` fields only; there is no resolver for
  `device.ip` / `wifi.ssid` / `device.heap`. The `metric_value` resolver added
  this session deliberately stays pure over `boat::View` — device state needs a
  *separate* resolver with access to `net::` / heap (device-only).
- **No `computed`/trip source kind**, no accumulation state anywhere.
- **No `status-list` element** (the 13-row status screen exceeds maxTiles).
- **`size` attr is unspecified.** The capabilities catalog
  (`midl/cpp/include/yb_midl_catalog.h` `A_NUM`/`A_WIND`/…) lists `"size"` as an
  allowed attr, but `midl/schemas/yb-midl-config.schema.json` defines **no type
  or enum** for it, and `src/midl_render.cpp map_element` never reads it. So the
  "depth hero-size" sub-task is blocked on first *defining* `size` (enum
  `small|medium|large|hero`? a scale number?) across schema + TS validator + web
  renderer + firmware painter.

**Recommended slicing (HW session):**
1. `size` first — smallest, self-contained once defined. Define the enum in the
   MIDL config schema + TS validator + web `paint.ts`, regen capabilities, then:
   firmware adds a trailing `int8_t size_class` to `MetricBinding` (gnu++11
   aggregate rule — trailing, value-init 0), `map_element` reads it,
   `paint_numeric_body` scales the hero font (reuse the existing zoom
   `fit_hero_scale` path). Build-verify + one screenshot.
2. **trip** — implement the accumulation as a *pure, host-tested* module first
   (`trip.cpp`: integrate distance from SOG·dt, elapsed, avg/max), independent
   of rendering. Decide product semantics up front (which speed; reset cmd;
   NVS persistence). Then expose via a `computed` source kind + bind to numeric
   tiles. The pure module is the one piece that can be written+tested without HW.
3. **status** — add a `local`/device-state resolver (device.ip/ssid/heap/uptime)
   and a `status-list` element (scrollable rows). Largest; do last.

### T6 — HUD/wind composites (mostly DONE)
compass / windrose / windsteer / autopilot all render as composites. Remaining
polish only (per handoffs): wind_steer density; optional CTS secondary line on
nav/route (needs a secondary/`dir` binding in `map_element`). Not blocking.

### T7 — LittleFS flash persistence (HW-gated)
Needs a `partitions.csv` change — **must be size-checked against the OTA + app
partitions on the real 16 MB flash** (current app is 2.20 MB / 33.6%, so there's
room, but the A/B + OTA layout must be verified on hardware, and a bad table
bricks boot). `midl_store` (factory rodata / current / last-known-good) + boot
load + boot-loop counter revert (NVS). Cannot be validated without HW.

### T8 — Manager MIDL push (cross-repo: `../signalk-espdisp-manager`)
Slice 5 of `2026-06-19-yb-midl-manager-migration.md`. Manager validates a doc
vs the device `/api/midl/manifest`, migrates to the device class/version, and
POSTs to `/api/midl/config` with the device token — **device must accept that
token on the POST (currently web-auth only).** Separate repo; needs a live
device to test the link.

### T9 — Cutover (depends on T5–T8 + soak)
Default build boots the full MIDL set from flash; retire legacy `screen_*.cpp`
where MIDL covers them (config screens stay native); soak + screenshot parity.
Inherently needs HW + a soak run (`espdisp soak`).

## Also found this pass (separate, pre-existing)

- **`make sim` is broken on `main`.** The `sim` / `sim-midl` / `sim-screens`
  envs compile `ui_layouts.cpp` but their link filter / `sim/stubs.cpp` was
  never updated when the wind_steer/aphud/button composites were added — they
  fail to link on `ui::build_centerzero_strip`, `net::dispatchCommand`,
  `g_pointer_dragging` (12 refs in committed HEAD; unrelated to this branch).
  Fixing `make sim` would also restore host-side visual validation of the
  `ui_layouts` render path. Quick win for a future pass: add those stubs to
  `sim/stubs.cpp` (+ `ui_compass.cpp` or a stub) in the three sim filters.
