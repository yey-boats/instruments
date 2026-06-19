# YB-MIDL — Yey Boats Marine Instrument Definition Language (Runtime Path, Design)

**Date:** 2026-06-19
**Status:** Design / for review
**Author:** Yey Boats Project

## 1. Problem & goal

Today's screens are a mix of (a) a runtime-interpreted layout config
(`layout::Config` → `Tile` with `widget`/`primary_path`/`zoom`) and (b)
a closed set of fixed `TemplateId` layouts plus several **bespoke
fullscreen HUD screens** (`screen_wind.cpp`, `screen_autopilot.cpp`,
`screen_steering.cpp`) whose markers, thresholds, and composition are
hardcoded in C++.

Goal: generalize the views into a **generic dashboard rendering model**
where every screen is a declarative config of **pre-built elements**,
each element binds its fields to **sources**, carries **style/format**
settings, and (for controls) **actions**. The same config drives
**multiple renderers**: a web preview now, the ESP32 firmware, and — in
future — mobile apps, other device types, and watches.

### Decisions locked (this session)

- **Name & formalism:** the model is **YB-MIDL** (Yey Boats Marine
  Instrument Definition Language). Its **capability manifest is a static,
  compile-time artifact generated from the firmware's single C++ element
  catalog** (versioned per build, served verbatim at `/api/diag`); config
  documents are validated against it by a JSON-Schema document grammar +
  a capability-satisfaction validator before posting (§3.6). Capabilities
  codegen is build-time and ≠ the tabled *config* codegen (§9).
- **Path:** runtime-interpreted config. The compile-time/codegen "bake"
  approach (signed base static lib + per-config screens built on the
  SignalK server/cloud, OTA'd) was **evaluated and tabled** — see §9. It
  buys RAM/footprint, not frame rate, and adds a compiler to the config
  loop; revisit only if config grows to define logic (computed
  fields/custom painters).
- **Format:** own lean JSON as the device-canonical wire format (sized
  for the POD / 512-byte BLE / PSRAM constraints), YAML accepted as
  authoring sugar (existing `/api/dashboard/config.yaml` alias). Borrow
  *concepts* from Grafana (field defaults+overrides, thresholds) and
  Home-Assistant Lovelace / ESPHome `lvgl:` (nested stacks, reusable
  styles) — adopt **no** external schema wholesale.
- **Layout:** named **presets are macros** that expand to a recursive
  **split/grid tree**; the editor and power users can also edit the tree
  directly.
- **Renderer v1:** **web preview first** to iterate the schema fast; keep
  ESP32 on the current path until the schema settles, then port
  `layout_renderer`.

## 2. Current state (baseline to generalize from)

The firmware is already ~70% of a generic model:

- **Config model** (`include/layout.h`): `Config → Screen[8] → Tile[4]`
  + `AlarmRule[8]`. Tiles already carry the editor shape (`widget`
  string, `primary_path`/`secondary_path`, `zoomable`/`zoom`).
- **Renderer indirection** (`src/layout_renderer.cpp` → `ui::layouts`):
  `Tile.widget` string → `WidgetKind` enum → painter dispatch `switch`.
  This is the "pre-built element + bound content" seam.
- **Capability manifest** (`/api/diag`, `src/capabilities.cpp`): the
  device already self-describes widget types, attrs, units, glyphs,
  fontSizes, and bounds to the editor.
- **Transport** (`src/web.cpp`, `src/layout_loader.cpp`,
  `src/signalk.cpp`): JSON canonical + YAML alias; live push via
  `network.yeyboats.configPush` trigger + REST fetch; `PUT /api/layout`
  (≤32 KB, queued to UI task as a PSRAM blob); last-good fallback.
- **Headless sim** (`sim/sim_main.cpp`): proves config-geometry → LVGL →
  snapshot, with no-overlap validation. Currently consumes a hardcoded
  `ScreenVariantSpec`, not JSON.

### 2.1 Current control catalog (Table A — what renders today)

| Element | Shows | Inputs | Configurable today | Status |
|---|---|---|---|---|
| Numeric | big value + context | 1 path + ≤4 extras | title, unit, precision, font, accent, zoom | shipped |
| Text | multi-line string | 1 path | title, accent | shipped |
| Gauge | 270° arc + center % | 1 scalar + min/max | range, unit, color | shipped |
| Bar | horizontal fill | 1 scalar (0–100%) | range, color | shipped |
| Compass | heading dial + markers | 1 primary + extras | 3 markers (HDG/COG/CTS) | shipped |
| WindRose | AWS center + wind markers | AWS + AWA/TWA | 2 markers, bow-up | shipped |
| RoundInstrument | circular bezel readout | 1 primary | (needle = future) | partial |
| TrendChart | rolling 60-pt line | 1 scalar | auto Y-scale | template |
| Autopilot | state pill + ±1/±10 | AP state + target | buttons (hardwired) | shipped |
| Button | tap bubble | static label | label, accent | no action binding |
| StatusList | label:value rows (≤8) | N paths | rows | template |
| AlertFocus | threshold-colored value | 1 scalar | thresholds (hardwired) | partial |
| Wind/AP/Steering HUD | composed dials + strips | 5–8 fields | per-screen hardcoded | bespoke |

Fixed layout templates today (`TemplateId`): QuadGrid (2×2/3×2),
HeroPlus, SplitPair, StatusList, RoundInstrument, TrendChart, AlertFocus;
stubs ControlConsole, RouteProgress, SetupForm.

**Generalization blockers:** layouts are a closed enum (no arbitrary
nesting/MxN); buttons have no action payload; markers/thresholds/zones
are partly hardwired in C++; fullscreen HUDs are bespoke `screen_*.cpp`.

## 3. Target model

```
Dashboard
 ├─ settings        (default screen, theme, demo period)
 ├─ defaults        (field defaults: units, colors, fonts — Grafana-style)
 ├─ Screens[]
 │   ├─ layout      base split/grid tree → leaves hold Elements
 │   └─ variants[]  optional per-resolution-class layout (its own tree
 │                  and element selection) — responsive breakpoints
 └─ Alarms[]
```

### 3.1 Element schema (the "content per element")

```
Element =
    type        catalog id: single-value | text | gauge | bar | compass |
                windrose | windrose+values | steering+values | round |
                trend | status | alert | autopilot | button | toggle | icon
  + bindings    a source per field — SignalK path, device-local, or other
                non-SignalK source: { value, angle, secondary, ... }
  + style       { color, frame, font, align }
  + format      { unit, precision, range:[min,max], zones:[...] }
  + markers[]   (dials only) { path, glyph, fill, color }
  + action      (controls only) { kind: put|nav|command,
                                  target: <path>|<screenId>|<name>,
                                  value: <literal> }
  + zoom        auto | <screenId> | none
  + overrides   (optional) per-element override of `defaults`
```

`defaults` + per-element `overrides` is the Grafana field-config pattern:
declare style/format once, reference everywhere, override per element.

**Sources are not SignalK-only.** A binding's source may be a SignalK
path, a **device-local source** (an onboard sensor / GPIO the device
exposes), or another non-SignalK source. The capability manifest
advertises which source kinds — and named local sources — a target
offers, so the validator can check bindings against them. Combined with
the shared element pool + per-placement overrides (§3.3, §10), the *same*
pooled element can bind a device's local source on one device and a
SignalK path on another: the placement carries the per-device source
override.

### 3.2 Proposed control catalog (Table B — generalized)

★ exists today · ☆ new. Categories: Value, Dial, Chart, Indicator,
Control, Composite, Decoration.

| Element | Cat | Inputs | Key settings | New work |
|---|---|---|---|---|
| ★ SingleValue | Value | 1 path | name, unit, precision, color, frame, font | rename of Numeric |
| ★ Text | Value | 1 path | name, color, wrap | — |
| ★ Bar (h/v) | Value | 1 path + range | range, zones, color, orientation | add vertical |
| ★ Gauge | Dial | 1 path + range | range, **zones**, threshold colors, unit | promote zones to config |
| ★ Compass | Dial | heading + markers[] | **markers** (glyph/fill/color/path), ref-mode | promote markers to config |
| ★ WindRose | Dial | AWS + AWA/TWA | + heading-up mode, current/tide arrows | — |
| ☆ WindRose+Values | Composite | wind + N values | rose + ringed value tiles | generalize `screen_wind` |
| ☆ Steering+Values | Composite | HDG/CTS/XTE/VMG | compass + XTE strip + value tiles | generalize `screen_steering` |
| ★ Round | Dial | 1 path | needle, range, ticks | finish needle |
| ★ Trend | Chart | 1–N paths | window, y-scale, series color | multi-series |
| ☆ Sparkbar/Histogram | Chart | 1 path | window, bins | new painter |
| ★ Status | Indicator | N paths | rows, label/value | — |
| ★ Alert | Indicator | 1 path + thresholds | **config thresholds**, blink | promote thresholds |
| ☆ LED/Lamp | Indicator | 1 path + rule | on/off color, label | new |
| ★ Autopilot | Control | AP state/target | step sizes, engage/standby | wire to action model |
| ★ Button | Control | — | **action** (put/nav/command) | wire action payload |
| ☆ Toggle/Switch | Control | 1 path + action | on/off PUT | new |
| ☆ Stepper | Control | 1 path + action | step, bounds | new |
| ☆ Icon/Image | Decoration | optional path | static glyph/logo | new |

### 3.3 Layout grammar & presets

Layout is a recursive tree; presets are named macros that expand to it.
Each leaf holds one Element.

```
node := leaf
      | split{ dir: row|col, children: [node, ...], weights?: [..] }
      | grid{ rows, cols, cells: [node, ...] }
```

| Preset | Expansion | Today? |
|---|---|---|
| Full-screen value | `leaf` | ✓ (Hero/Alert) |
| `{1,{2,3}}` | `split row [ leaf, split col [leaf,leaf] ]` | new |
| `{1,{2,3,4}}` | `split row [ leaf, split col [3 leaves] ]` | new |
| `{1,{2,3,4,5}}` | `split row [ leaf, grid 2×2 ]` | ≈ HeroPlus |
| `MxN` | `grid(M,N)` | partial (2×2/3×2) |
| `{{2\|3\|4},1,{2\|3\|4}}` | `split col [ band(2–4), hero, band(2–4) ]` | bespoke only |

The renderer walks the tree and assigns pixel rects per device
resolution class (the existing display-class table in
`docs/layout-editor-guide.md` stays the source of per-board grid sizing).

**Resolution-aware (required).** Each display-resolution class has its
own limits — max tiles, grid bounds, nesting depth, and even which
elements fit — and may warrant its **own** layout, not just a reflow of a
smaller one. A screen therefore carries a **base** layout plus optional
**per-class variants** (responsive breakpoints): if the target class has
a variant, it is used; otherwise the base reflows within that class's
limits. The capability manifest (§4) publishes each class's supported
presets and limits, and the editor offers only what the target class
allows. The full multi-resolution definition lives in the authored file;
the **manager resolves it to the target device's single class** and
serves/pushes that one resolved layout — so the device POD never carries
more than its own resolution's layout, however many classes the file
describes. The web preview resolves any class locally for side-by-side
preview.

### 3.4 Actions

`put` writes a SignalK delta (e.g. `steering.autopilot.state =
"engaged"`); `nav` switches screens (reuses `view`/zoom); `command` runs
a named console command through the existing `net::dispatchCommand`
funnel. No arbitrary code (no ESPHome-style lambdas) — actions are a
closed, validated set, since config arrives over BLE/network.

### 3.5 Definition file, presets, and import/export

The unit of authoring is a **dashboard definition file** (YAML or JSON,
losslessly interconvertible). The **manager UI is the sole authoring
surface** and treats this file as the manipulated object: **save,
export, import, restore**. YAML↔JSON round-trip must be lossless (export
then re-import yields an equivalent definition), so YAML is a
first-class storage face, not merely sugar.

**Presets are themselves savable/exportable artifacts.** Beyond the
built-in preset macros (§3.3), a user can save a layout — or a styled
element group — as a named preset into a **preset library**, export it
as a YAML/JSON file, and restore/import it into another dashboard or
device. Presets are resolution-class-tagged so the library offers the
right ones per target.

Round-trip safety reuses the device's existing verbatim-JSON store
(`s_last_json`); the authored file of record lives in the manager /
SignalK config store, and the device receives a **resolved
single-class projection** of it.

### 3.6 Formal language & validation (YB-MIDL)

The model is named **Yey Boats Marine Instrument Definition Language
(YB-MIDL)**, specified by three normative artifacts plus a validation
pipeline — all host- and browser-runnable, so a document is **formally
tested before it is posted to a device**.

**Capabilities are static, built at compile time.** A target's
capabilities are fixed by its firmware build, not discovered at runtime.
The single source of truth is the firmware's **compiled element catalog**
— one declarative C++ table that the painter dispatch *also* consumes, so
the manifest can never advertise an element the firmware cannot draw. At
build time a generator emits the **MIDL capabilities document**
(`schemas/gen/yb-midl-capabilities.<board>.json`, versioned by firmware
version + MIDL schema version) from that catalog; the firmware embeds the
generated bytes and serves them **verbatim** at `/api/diag`. The same
generated JSON is what the manager imports and what host tests use as the
satisfaction fixture — **one source, many consumers**. A host test
asserts the painter dispatch covers exactly the declared catalog (no
missing or orphan element types). This build-time codegen produces the
*capabilities descriptor only*; it is unrelated to the tabled idea of
compiling user *configs* into firmware (§9).

**Generation pipeline — C++ → MIDL → bindings.** The catalog is the
upstream source; **MIDL is the neutral contract hub**; downstream
language bindings are generated, never hand-maintained:

```
C++ element catalog   (single source of truth, beside the painters)
      │  build-time generator
      ▼
MIDL artifacts:  yb-midl-capabilities.<board>.json   (versioned manifest)
                 yb-midl-*.schema.json                (manifest + config grammars)
      │  schema → bindings generator
      ▼
TypeScript / JS types + validator   (manager UI, web renderer)
      └─ (future) Swift / Kotlin      (mobile / watch)
```

The manager's typed `Config` / `Manifest` interfaces and the web
renderer's element types are **generated from MIDL** — which is itself
generated from the firmware catalog — so no hand-written binding can
drift from what the device actually renders. A new renderer platform adds
a bindings target, not a new source of truth.

**Three artifacts**

1. **Capability manifest schema** —
   `schemas/yb-midl-capabilities.schema.json` (JSON Schema 2020-12). The
   grammar of a capabilities document: per resolution class, the element
   types a target renders (each with allowed bindings, attrs, units,
   ranges, marker glyphs), supported presets, layout limits (`maxTiles`,
   `maxDepth`, grid bounds), fonts, action kinds, themes, plus
   `firmwareVersion` / `midlVersion`. The generated manifest MUST validate
   against it.

2. **Config document schema** — `schemas/yb-midl-config.schema.json`. The
   grammar of a dashboard: `dashboard → defaults → screens → {layout,
   variants[]} → node-tree → element(...)`, alarms, presets. Structural
   well-formedness only.

3. **Capability-satisfaction validator** — the semantic layer JSON Schema
   cannot express. A config is *admissible for target T at class C* iff
   every element type, attr, unit, glyph, preset, action kind and layout
   shape it uses is within `T`'s manifest for `C`, and within `C`'s
   numeric limits and the device POD bounds. Pure function
   `validate(config, manifest, class) → { ok, errors[] }`.

**Framing.** The (static) manifest *induces a per-target grammar* — it
selects which productions are legal. A document is valid iff it is a
sentence in the language that target's manifest generates:
`config ∈ L(manifest @ class)`. Because the manifest is versioned, a
config admissible on firmware A may be rejected for firmware B; the
manager always validates against the *target's* capabilities version.

**Layout mini-grammar (EBNF)** — the recursive core, bounded per class by
manifest limits:

```
layout     = node ;
node       = leaf | split | grid | preset-ref ;
leaf       = '{ "element": ' element ' }' ;
split      = '{ "dir": ("row"|"col"), "children": [' node {',' node} ']'
                 [ ', "weights": [' number {',' number} ']' ] '}' ;
grid       = '{ "rows": int, "cols": int, "cells": [' node {',' node} '] }' ;
preset-ref = '{ "preset": name [, "slots": [' element {',' element} '] ] }' ;
```

`preset-ref` expands to a `node` per §3.3; the expansion is re-validated
to fit `maxTiles` / `maxDepth` for the class.

**Validation pipeline (all must pass before POST):**
```
doc (YAML | JSON)
 1. canonicalize    YAML → JSON, lossless
 2. structural      validate vs config schema               → well-formed?
 3. expand presets  macros → concrete node-tree
 4. satisfy         check vs target manifest @ class         → admissible?
 5. bounds+geometry POD string/array maxima; no-overlap, in-bounds
 → POST iff 1–5 pass; else reject with path-addressed errors
```

**One logic, four sites (all from the same C++→MIDL→JS source):**
- **Manager plugin / UI (TS/JS):** authoritative validation **and
  multi-class resolution** for delivery; live validation while authoring;
  export/post of an invalid document is blocked.
- **Device-served standalone validator:** the device serves the generated
  static JS validator + schemas + its own embedded manifest as HTTP
  assets, so a browser pointed straight at the device can parse, validate
  (against that device's class), and apply a pasted config **without the
  plugin** — same compile-time-derived JS, no second implementation.
- **Host tests / CI (`pio test -e native`):** schemas + validator run a
  fixture corpus (valid pass; invalid rejected with expected errors);
  YAML↔JSON round-trip asserted lossless; the generated manifest
  validates against the capability schema; dispatch-covers-catalog
  asserted.
- **Device (defensive parse):** `layout::parse()` keeps its
  `memset`/bounds guards as the last line of defence; a conformant
  manager never sends a document that would trip them.

**Cross-implementation parity.** The JSON Schemas are language-neutral
(validate in JS and native). The small satisfaction function is pinned by
a **shared golden corpus** (`test/fixtures/yb-midl/`: configs + manifests
+ expected verdicts) that both the manager (JS) and host implementations
run — so they cannot silently diverge.

### 3.7 Storage, library & rollback tiers

Three tiers, most-local first:

**Device (rollback chain).** Each device retains three configs and can
fall back without external help:
- **current** — the running variant (downloadable / uploadable);
- **last-known-good** — the last config that parsed and rendered cleanly
  (today's `s_last_json`), restored automatically on a bad apply;
- **factory default** — the baked-in default (`load_default()`), the
  final rollback floor.

**Vessel (SignalK config store).** Holds a **historical library**,
**folder per instrument** (device), versioned. Any stored config can be
either **applied to supporting devices** — gated by the
capability-satisfaction check against the target's manifest — or used as
a **base for authoring** a new document. This is where a vessel's display
history and reusable starting points live, surviving device replacement.

**Fleet (future).** A shared registry / marketplace for presets and whole
dashboards across boats and owners — deferred (§8); file export/import
(§3.5) is the manual cross-boat path until then.

## 4. Renderer architecture

```
        config (JSON canonical / YAML sugar — one source of truth)
                              │
        ┌─────────────┬───────┴────────┬───────────────┐
   Web preview     ESP32 (LVGL)     Mobile/Watch    Headless sim
   (v1 target)     (port later)     (future)        (validation)
        └──── each advertises capabilities via /api/diag manifest ────┘
```

- Every renderer maps catalog `type → native painter`; the firmware
  reuses today's compiled painters (`ui::layouts`).
- The **capability manifest** is the contract: it publishes the
  supported catalog, presets, grammar features, units, glyphs,
  fontSizes, and bounds. The editor only offers what the *target* device
  reports — so a watch renderer that lacks `windrose+values` simply isn't
  offered it.
- The manifest is **per resolution class**: for each class a target
  supports it publishes the allowed presets, max tiles, grid and nesting
  limits, and the element subset that fits — so the editor constrains
  authoring (and per-class variants) to what each target can actually
  render.
- The manifest is a **static, build-time-generated, versioned artifact**
  (from the firmware's single C++ element catalog — §3.6), served
  verbatim at `/api/diag` and imported unchanged by the manager and host
  tests, so the contract cannot drift from what the firmware renders.

## 5. Scope of v1 (incremental, on existing code)

| # | Change | Touches |
|---|---|---|
| 1 | Generalize `layout::Tile` → `Element` schema; promote markers/thresholds/zones/actions into config; add `defaults`+`overrides`; generalized **source** kinds (SignalK + device-local/non-SignalK) | `layout.h`, `layout.cpp`, `layout_renderer.cpp` |
| 2 | Replace `TemplateId` enum with split/grid grammar + preset macros | `ui_layouts.*`, `layout_renderer.cpp` |
| 3 | Wire the action model (Button/Toggle/Autopilot) through the dispatch funnel | `ui_layouts.cpp`, `net`/dispatch, `web.cpp` |
| 4 | Single **C++ element catalog** (source of truth) + build-time **capabilities generator** → versioned per-class MIDL manifest, served verbatim at `/api/diag`; dispatch-covers-catalog test | `src/`(catalog), `capabilities.cpp`, `tools/`, CI |
| 5 | **YB-MIDL schemas + MIDL→TS/JS bindings generator + satisfaction validator** + golden corpus, shared by manager, web & host tests; **device-served static validator bundle** (HTTP asset) for plugin-less direct authoring | `schemas/`, `tools/`, `test/test_midl`, `web.cpp` |
| 6 | Per-resolution **variants** (base + breakpoints) + manager-side resolution to the target's single class | `layout.*`, manifest, manager |
| 7 | Definition-file **save / export / import / restore** with **validate-before-post**; **rollback tiers** (device current / last-known-good / factory-default) + SignalK **historical library, folder-per-instrument** (lossless YAML↔JSON) | manager UI, `web.cpp`, `layout_loader` |
| 8 | **Web renderer** consuming the same JSON, multi-resolution preview (interactive sibling to the sim) | new web surface + shared manifest |
| 9 | (later) Port `layout_renderer` to the grammar on-device | `layout_renderer.cpp` |

**v1 deliverable = items 1–8** (schema + grammar + actions + generated
per-class manifest + YB-MIDL schemas/validator + resolution variants +
import/export + web renderer). Item 9 (firmware port) is a follow-on once
the schema is proven in the web preview.

## 6. Constraints to preserve (firmware memory traps)

The on-device schema and any new POD growth must respect CLAUDE.md:
- `layout::parse()` keeps `memset(&out,0,sizeof(out))` — never
  `out = Config{}` (34 KB stack temp boot-loops the device).
- Live `Config` stays PSRAM-allocated; growth must not push internal-SRAM
  scratch (NimBLE starvation).
- BLE attribute values cap at 512 bytes → large configs come via the
  SignalK REST endpoint + `layout-fetch`, not BLE reads.
- No large scratch structs on task-callback stacks (web/GATT handlers);
  use `static` + `memset` or PSRAM.
- Element/Screen/Tile bounds stay fixed-size arrays (no dynamic alloc in
  the POD).

## 7. Testing

- **Host tests** (`pio test -e native`): schema parse — presets expand to
  the expected tree; element bindings resolve; action validation rejects
  unknown kinds/targets; round-trip JSON stability.
- **Sim harness** (`make sim`): each preset renders with no tile overlap
  and in-bounds at 480/800/1024 classes.
- **Web renderer**: snapshot/parity tests against the sim geometry so
  preview matches device.
- **Capability manifest**: editor-parity test (offered options ⊆ device
  capabilities).
- **YB-MIDL conformance** (host + CI): the generated manifest validates
  against the capability schema; the painter dispatch covers exactly the
  declared catalog (no missing/orphan types); a golden corpus of valid
  configs passes and invalid configs are rejected with the expected
  path-addressed errors; YAML↔JSON round-trip is lossless; the JS
  (manager) and native satisfaction validators agree on every corpus
  case.

## 8. Non-goals (v1)

- **Config/screen** codegen — compiling user *configs* into firmware
  (see §9). Note the *capabilities descriptor* IS build-time generated
  from the C++ catalog (§3.6); that is a different, in-scope thing.
- Arbitrary computed fields / expression language (would justify
  revisiting codegen — see §9).
- Mobile/watch renderers (future; the manifest reserves room for them).
- Relicensing or changing transport security model.

## 9. Tabled alternative: compile-time "bake" (for the record)

Evaluated and deferred. A signed static base lib + per-config screens
compiled (on the SignalK server or cloud) and OTA'd would buy **RAM,
footprint, and determinism** — but:
- **Not frame rate.** Steady-state is bandwidth/rasterizer-bound; both
  paths feed the same compiled painters + RGB DMA. Per-frame binding
  dispatch is µs against a multi-ms frame.
- Adds a **compiler to the config loop** (reboot + OTA per change; signing
  key placement problem: central-but-online vs per-device key).
- The LVGL tree is built at runtime in *both* paths, so even apply-latency
  only improves by the JSON-parse delta (a few ms).

**Revisit trigger:** if the config language grows to *define logic*
(computed/derived metrics, custom painters), codegen becomes the
*enabling* technology (an on-device interpreter would be slower and
heavier). Until then, the runtime path wins on live editing, offline
authoring, and safe fallback.

## 10. Resolved & remaining questions

**Resolved this session:**
- **Authoring face** — the manager UI is the sole author; the manipulated
  object is a YAML/JSON **definition file** with save/export/import/
  restore (§3.5). YAML is first-class (lossless round-trip), not just
  sugar.
- **Per-resolution variants** — required. Each resolution class has its
  own limits and may carry its **own** layout; screens hold a base layout
  + per-class variants, resolved to the target's single class by the
  manager (§3.3).

- **Resolution & validation site** — runs **manager/plugin-side**
  (authoritative, multi-class). The **device additionally serves the
  generated static JS validator + its embedded manifest** (HTTP assets),
  so a standalone browser can parse/validate/apply a config directly to
  that device with no plugin — same compile-time-derived JS (§3.6).
- **Element sharing** — **shared per-screen element pool + per-placement
  overrides** (§3.3); this is also what lets a placement bind a device's
  **local / non-SignalK source** per device (§3.1).
- **Library & rollback** — device holds current / last-known-good /
  factory-default; SignalK holds a historical, folder-per-instrument
  library (apply-to-supporting-devices or authoring base); fleet
  registry/marketplace is future (§3.7).

**Still open:**
- **Source taxonomy** — enumerate the non-SignalK / device-local source
  kinds the binding model and manifest must support (onboard sensors /
  GPIO, direct NMEA2000, constants, computed?). Detailed during the
  schema work (scope item 1).
