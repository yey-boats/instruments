# Dashboard Program Design

**Date:** 2026-06-12
**Status:** Approved design, pending implementation plans
**Scope:** Four sequenced sub-projects: stability hardening, render-pipeline unification, glass-cockpit visual redesign, push-live loadable dashboards.

## Context

The firmware (v0.3.5 era) renders SignalK boat data on a Sunton ESP32-4848S040
(480×480). Analysis on 2026-06-12 found:

- **Two parallel render paths.** Editor JSON renders via
  `layout_renderer.cpp` → tile screens, with 9 working widget painters living
  inside `ui_layouts.cpp`. The manager RenderPlan path
  (`manager_config.cpp` → `manager_screens.cpp` → `widget_registry.cpp`) has
  validation/clamping but only 3 of 9 widgets implemented (numeric, text,
  bar); gauge, compass, windRose, trend, button, autopilot are stubs.
  Every visual change currently lands twice or diverges.
- **Stability**: device still exhibits stalls/reboots and WiFi-specific
  failures. During this design session the bench device was stalled and its
  HTTP API unreachable at `espdisp.local`. Compiler stack canaries are OFF
  (`CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y`, sdkconfig:221). The
  `/api/state` → `/api/diag` split from the 2026-06-03 analysis is done; the
  WiFi half-up issue (S4) and manager heartbeat `-1` loop (S5) were never
  confirmed fixed.
- **Visuals**: current dashboards are functional engineering UI (flat navy
  tiles, small cyan caps), not a commercial-MFD look.
- **Pending cleanups**: `screen_settings.cpp` repetitive segmented-control
  code (D3), `touch_cal` not release-gated (D4), `manager.cpp` at 2157 lines
  (Σ2).

## Approved decisions

| Decision | Choice |
|---|---|
| Sequencing | Foundation-first: SP1 stability → SP2 unification → SP3 redesign → SP4 push-live. Each sub-project: spec → plan → implement → soak-gated done. |
| Render architecture | **Painter library + RenderPlan canonical.** Shared painter module; RenderPlan is the device's single render input; editor JSON converted by a pure, host-testable converter. |
| Visual language | **B — Glass cockpit** (Garmin school): bordered rounded cells, subtle vertical gradient, centered values, semantic value colors, unit badges. Approved mockups archived in `docs/superpowers/specs/assets/2026-06-12/`. |
| Layout archetypes | Standard 2×2, dense 3×3, master-detail (hero span + stack + strip) must all be composable. |
| Style source | **Consolidated style config**: all style values in one source (compile-time acceptable), code-generated for firmware and editor. No inline style constants in painters/screens. |
| Loadable-dashboard UX | Edit on server, push live: save in editor → device hot-reloads in seconds; device caches last config for offline boot. |
| Push mechanism | SignalK delta on a per-device config-version path over the existing WS subscription; debounce ~2 s; 30 s manager poll stays as fallback. No new socket. |
| Stability method | Evidence-first: capture live failures before changing code; soak rig gates every "fixed" claim. |
| Lab deployment | Dev plugin versions deploy freely to mythra-nav (lab SignalK host); dev firmware via `make ota` to the bench device. |

## SP1 — Stability, evidence-first

**Goal:** 24 h soak with zero reboots and zero unrecovered stalls; remaining
failure modes named and measured, not guessed.

1. **Evidence capture before any reflash.** *Done 2026-06-12* —
   `docs/reports/stability-evidence-2026-06-12.md`. The device is unreachable
   from every angle (workstation mDNS fails; the manager, on the device's own
   subnet, times out hitting it). The manager registry `lastSeen` is
   2026-05-28 with a stale cached IP. Next on-device step once physically
   reachable: pull `/api/diag`, prevboot RTC ring, and BLE logs to classify
   the reboot signature.

2. **Discovery/re-resolution fix (surfaced by the capture).** mDNS is
   disabled on the device and the manager trusts a cached DHCP IP, so an IP
   rotation orphans the device. Re-enable mDNS (or periodic announce) on the
   device; on `device_unreachable`, the manager re-resolves via mDNS /
   discovery sweep instead of the stale IP. Restore the heartbeat loop so the
   registry tracks current firmware (it has been blind since 2026-05-28).
3. **Compiler stack canaries on**: `CONFIG_COMPILER_STACK_CHECK_MODE_STRONG`
   (currently NONE). Converts silent stack corruption into named crashes
   during soaks. Accept the small CPU/flash cost in dev builds; measure
   before deciding for release builds.
4. **Soak rig** (`tools/espdisp.py soak` or equivalent): scrape
   `/api/state` + `/api/diag` every 30 s to JSONL; detect uptime regression
   (reboot), `sk` last-update age growth (stall), heap trends, WiFi RSSI;
   end-of-run verdict table. **Runs on `mythra-nav`** (same subnet as the
   device) — the workstation cannot reach the device directly (routed,
   different subnet, no cross-subnet mDNS), per the 2026-06-12 capture.
5. **Network-health monitor (S4).** `WiFi.status()` lies in the half-up
   state. New pure module `net_health` consumes actual traffic signals
   (WS frame age, HTTP request outcomes, RSSI presence) and produces a
   verdict (`OK / DEGRADED / DEAD`) via a host-testable state machine. The
   existing 3-tier stall ladder keys off this verdict instead of the driver
   status. Escalation: WS restart → WiFi reconnect → (existing) guarded
   reboot.
6. **S5 labeling.** Manager heartbeat distinguishes pre-flight refusal
   (NotProvisioned / LowHeap / WifiDown) from transport failure in
   `/api/diag` counters, so future analysis has labeled data.
7. **Rideshare cleanups:** gate `touch_cal` out of release builds (D4);
   `Segmented` factory for `screen_settings.cpp` (D3). The `manager.cpp`
   split (Σ2) waits for SP2, which opens that file anyway.

**Done-gate:** 24 h soak on the bench device against mythra-nav with the rig
attached; report committed to `docs/reports/`.

## SP2 — Render unification

**Architecture (one pipeline, two doors):**

```
editor JSON ──> editor_plan converter ──┐   (pure, host-tested)
 (bench POST / plugin store)            ├──> RenderPlan (PSRAM, memset-init,
manager fetch ──────────────────────────┘    validated + clamped)
                                              │
                                   manager_screens (LVGL tree build,
                                   UI task only via app::Command)
                                              │
                                src/ui/widgets/ painter library
                                create(parent, spec, theme)
                                update(handle, snapshot)   destroy(handle)
                                              │
                                5 Hz dirty-checked refresh from sk::data
```

- **Painter library**: the 9 painters move out of `ui_layouts.cpp` into
  `src/ui/widgets/` (one file per kind: numeric, text, bar, gauge, compass,
  windRose, trend, button, autopilot) behind the uniform interface above.
  `widget_registry` becomes the dispatch table over real painters; its stubs
  are deleted.
- **Converter**: `layout_renderer.cpp` is repurposed into
  editor-JSON → RenderPlan conversion (pure C++, host-testable). The device
  keeps accepting editor JSON on `/api/dashboard/config.json` through it.
- **Bounds**: `MAX_TILES_PER_SCREEN` 4 → 12; tile `colSpan`/`rowSpan` flow
  through RenderPlan end-to-end (needed for dense 3×3 and master-detail).
- **Constraints (standing memory traps):** RenderPlan/Config PSRAM-heap
  allocated; `memset` init, never struct-assignment temporaries; all LVGL
  mutation on the UI task via `app::Command`.
- Built-in screens do not migrate in SP2 (their look changes in SP3 anyway).

**Done-gate:** all 9 widget kinds render from a pushed config on the bench
device; converter golden tests green; 24 h soak repeats clean.

## SP3 — Visual redesign (glass cockpit)

- **Consolidated style config.** `style/tokens.json` is the single source of
  every style value: night+day palettes (cell border `#1f2d3d`, gradient
  `#101b29→#0b1320`, value `#eef4fa`, label `#8fa7bd`, unit-badge bg
  `#16222f`, semantic stbd `#36d399` / port `#ff5252` / depth `#4fc3f7`,
  warn/alarm/ok), chrome metrics (border 1 px, radius 10 px, paddings,
  badge geometry), and the typography map. Build-time codegen produces
  `include/ui_style_gen.h` (constexpr, consumed by `ui_theme` + painters)
  and a tokens artifact for the plugin editor preview. Parity between
  editor and device becomes a codegen guarantee; the existing Playwright
  parity tests validate the generated artifacts stay in sync. Inline style
  constants in painters/screens are prohibited.
- **Cell chrome built once** in the painter-library root; every widget
  inherits border/gradient/label/badge placement.
- **Typography map** (within the enabled Montserrat set 14/20/28/38/48):
  2×2 → 48, dense 3×3 → 38, master-detail stack → 38, mini-strip → 20.
  A ~64 pt face for 2×2 is a deferred, ROM-measured decision after SP1/SP2.
- **Wind-dial painter** upgraded to mockup quality: tick bezel, port/stbd
  arc sectors, rotated needle, center readout.
- **Staleness is visual**: when a path's data is older than its timeout, the
  value grays and renders `--` (per-widget, from `sk::data` timestamps).
  A confident-looking stale number is a safety bug.
- **Top strip** (title / SK dot / time / voltage) as optional per-screen
  chrome flag in RenderPlan.
- **Built-in screens become factory presets**: dashboard, nav, depth, route,
  trip, status re-expressed as built-in RenderPlans rendered by the unified
  pipeline. Bespoke survivors initially: wind, autopilot, settings,
  diagnostics screens.
- **Editor preview** consumes the generated tokens; preview snapshots
  regenerate.

**Done-gate:** device screenshots of the three archetypes match the approved
mockups in language (palette, chrome, hierarchy); parity tests green; soak
repeats clean.

## SP4 — Push-live + editor depth

- **Push channel:** on save, the plugin emits a SignalK delta on a
  per-device config-version path. The device's existing SK WS subscription
  notices, debounces ~2 s, fetches config via the manager client, applies.
  30 s poll remains as fallback; the path works through the stall-recovery
  ladder.
- **Transactional apply:** snapshot the active RenderPlan (PSRAM) → build
  the new tree → on any failure (parse, alloc, painter create) destroy the
  partial build and rebuild from the snapshot. The old dashboard never
  stops rendering. Device acks with the active config hash immediately so
  the editor shows "live on device" within seconds. This closes the
  roadmap's transactional-rollback gap.
- **Editor depth:** drag/drop grid canvas with span resize; three starter
  templates matching the approved archetypes; existing SignalK path picker
  and live screenshot preview wired into the flow.

**Done-gate:** edit→save→on-device change measured under 5 s on the lab
bench; pulling power mid-apply leaves the device booting the last-good
config; Playwright e2e (save → push → mock-firmware fetch → ack) green.

## Error handling (program-wide)

- Every apply path keeps last-good + rollback (SP4 formalizes it for the
  full RenderPlan).
- Painter create failure → placeholder cell with error glyph + label, logged
  to the error ring; never a crash.
- RenderPlan version/display mismatch → reject, keep current, report coded
  status to the manager.
- Canary/watchdog events land in the prevboot ring; the soak rig flags them.

## Testing strategy

- **Host (Unity, `pio test -e native`)**: converter golden tests (standard,
  dense 3×3, master-detail spans, bound clamps, malformed input);
  `net_health` state-machine tests; extended `test_manager_config` for new
  bounds/spans; `widget_data_resolver` coverage for every editor-exposed
  path.
- **Plugin (Node + Playwright)**: editor save → push → mock-firmware fetch →
  ack e2e; preview parity snapshots against generated tokens.
- **Device**: `make ota-verify` per milestone; 24 h soak rig run is the
  done-gate for every sub-project.

## Backlog (suggested, not committed)

- Alarm/limit bands on widgets (the layout schema already carries alarms).
- Depth sparkline / trend widget completion.
- Auto day/night from sun position (lat/lon + time already available).
- Anchor-watch screen; MOB button widget.
- Multi-device preset assignment polish (plugin already has presets).

## Open points

- 64 pt numeral face: decide after SP2 with measured ROM headroom.
- Release-build policy for stack canaries: decide from soak data.
- Exact SignalK path for the config-version delta: pick during SP4 planning
  to fit SignalK conventions (candidate: a `plugins.espdisp.<deviceId>.*`
  self-vessel path).
