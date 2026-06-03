# SignalK plugin audit — 2026-06-04

Pre-redesign findings on `signalk/plugins/signalk-espdisp-manager/`.
Scoped to: stability already proven (overnight watch at 44+ min uptime
with 0 reboots while writing this). Focus is **efficiency,
simplicity, and discoverability** for the operator.

## Surface area

| File                | LOC | Role                                  |
|---------------------|----:|---------------------------------------|
| `index.js`          | 2 109 | HTTP routes, server-side rendering, form parsing |
| `lib/manager.js`    | 2 553 | Domain logic: devices, profiles, commands, firmware |
| `lib/store.js`      |   203 | Plugin-config persistence wrapper |
| `lib/util.js`       |    85 | Misc helpers (yaml, hash, escape) |
| `public/layout-editor.html` (new) | 373 | The MVP editor I shipped yesterday |
| **Total**           | **7 503** | |

Plus a fat test surface (24 files), `node_modules`, Playwright config.

## Findings

### 1. **Server-side HTML rendering is the bulk of `index.js`** — 737 LOC

27 `render*` functions dominate the file (1 049 / 2 109 lines = **50 %**).
This is the classic "we built a SPA in template strings" trap:

- 110 inline HTML fragments scattered through JS, each independently
  escaping, layouting, styling.
- `<style>` blocks embedded in `renderUiShell` (~80 lines of CSS).
- Forms hand-built with `field()`/`input()`/`select()`/`checkbox()`
  helpers that exist *only* to build HTML server-side.
- Six distinct page renderers (`renderOverviewPage`, `renderDevicesPage`,
  `renderDevicePage`, `renderDeviceConfigPage`, `renderPresetPage`,
  `renderFirmwarePage`) that all share the same dashboard data but
  format it differently.

**Cost**: every UI change touches HTML escape logic + form parser +
renderer. ~50 % of `index.js` is presentation, not logic.

**Cheap reduction path** (no breaking change): keep the existing routes,
but stop rendering inside `index.js`. Move all `render*` functions into a
small set of static HTML pages in `public/` that fetch the same data via
the JSON endpoints. The pages are *already* there in the API — the JSON
endpoints exist alongside every UI route. Result: ~700 LOC deletion.

### 2. **Routes are duplicated under `/ui/` AND root**

The plugin has 59 routes. **53 lines contain `/ui/`** patterns.
Looking at them:

- `GET /devices` ↔ `GET /ui/devices`
- `GET /profiles` ↔ `GET /ui/profiles`
- `GET /devices/:id/config` ↔ `GET /ui/devices/:id/config`
- `GET /devices/:id/live/status` ↔ `GET /ui/devices/:id/live/status`
- …same pattern for at least 10 endpoints

The `/ui/*` versions return HTML; the root versions return JSON. Each
pair has its own handler that re-implements the data shape and adds
HTML-escaping or form-parsing. The JSON handlers are the source of
truth; the HTML handlers should be **deleted** and the UI shells
served as static HTML that calls the JSON ones.

**Cheap reduction**: removing the 10-ish `/ui/*` handlers alone saves
~400 LOC and removes a constant maintenance cost.

### 3. **`importDashboardPreset` + `parseDashboardImport` are over-flexible**

The save endpoint accepts:

- `body.raw` as YAML or JSON string with content-type sniffing
- `body.dashboard` direct doc
- `body.preset` direct doc
- raw body as direct doc

…all coalesced through a tortured `if` chain. Looking at actual
callers: there's one caller (the form), one HTML test (Playwright),
and the new editor I shipped. **Two of the three pathways are unused**
in practice.

**Cheap reduction**: collapse to `body.dashboard` (the canonical shape)
and a `body.raw` YAML/JSON fallback for the import-form. Remove `preset`
key entirely.

### 4. **`generateConfig` does 8 things**

`manager.js::generateConfig` (78 LOC) builds the device-bound config:

- Resolves display dimensions
- Resolves widgets
- Resolves layout (picks the variant for the device)
- Resolves network identity (the bug we just fixed lives here)
- Resolves manager intervals
- Resolves SignalK target
- Resolves debug settings
- Resolves OTA confirm

Each is a method call already (`resolveWidgets`, `resolveLayout`,
`resolveDisplay`, …). The composition is fine. But the **8th step**
where it computes the hash + version is mixed with the assembly. If we
add screen presets next, this is the entry point we'll touch. Keep it
unified; just don't add to the inline duplication.

### 5. **No catalog of "all sane screens" exists yet**

The plugin's widget catalog (`pluginCapabilities`) lists widget *types*:
numeric, text, gauge, compass, windRose, trend, bar, button, autopilot.
But there's no curated set of **screen presets** that an operator would
want as their starting point (e.g. "dashboard with WIND/NAV/DEPTH/SYSTEM
tiles", "fullscreen wind", "autopilot control"). The firmware *expects*
specific screen IDs (`dashboard`, `wind`, `nav`, `depth`, `steering`,
`route`, `trip`, `status`) — the plugin doesn't even publish that list.

**This is the actual UX gap.** Add an `/api/presets/screens` endpoint
returning a curated catalogue per display class.

### 6. **Test surface (24 files) overlaps significantly**

`discovery.test.js`, `discovery-scan.test.js`, `discovery-claim-e2e.test.js`,
`device-udp-discovery.test.js`, `udp-discovery.test.js`,
`mdns-discovery.test.js` — six tests for discovery. Some are unit,
some integration; not clear which is the source of truth. Probably
fine to leave (they all pass and discovery is genuinely complex), but
flag for "if anything breaks here, the duplication is suspect."

### 7. **`store.js` is tiny and well-scoped** (203 LOC)

This is the boring-good kind of file. Persistence wrapper around
`app.savePluginOptions`. Nothing to change here.

### 8. **`util.js` is 85 LOC and exports `yaml`/`hash`/`escape`/`now`**

The `yaml` import is the wonky one — uses `js-yaml` for one path
(the form-paste import) and could be deleted along with the form.
If we keep the form-paste path, leave it. Otherwise that's a few KB
of dependencies.

## Concrete reduction targets

| Action | LOC saved | Risk |
|---|---:|---|
| Replace 27 `render*` functions with static `public/*.html` pages | ~750 | Low; existing JSON endpoints serve the same data |
| Drop `/ui/*` handlers, redirect to the static pages | ~350 | Low; just URL aliases |
| Collapse `importDashboardPreset` to 2 paths instead of 4 | ~30 | Low |
| Drop `js-yaml` if no longer needed | ~40 (+ deps) | Low |
| **Net target** | **~1 170 LOC (≈ 16 %)** | |

After this trim: `index.js` drops from 2 109 to ~940 LOC. Still big
but mostly routes + light glue.

## Plan

Three phases, each independently shippable:

### Phase A — screen preset catalog + `/api/presets/screens` (BEST USER VALUE)

This is what the user actually asked for. **Build this first.**

1. New file `lib/screen-presets.js` (~250 LOC):
   - `getScreenPresets(displayClass)` returns an array of preset
     screens for that display class. Display classes: `sunton-480`,
     `waveshare-4_3-800x480`, `waveshare-5-1024x600`,
     `waveshare-7-800x480`. (Add others as boards land.)
   - Each preset has `{ id, title, type, tiles[] }` matching the
     firmware's expected schema.
   - Curated set per class: **dashboard** (quad-grid),
     **fullscreen-wind** (windRose), **fullscreen-nav** (compass +
     numeric stack), **depth-temp**, **autopilot**, **route**, **trip**,
     **system**.
2. Endpoint `GET /api/presets/screens?displayClass=<x>`.
3. Endpoint `GET /api/presets/widgets` returning the widget metadata
   the editor needs (allowed metric paths per widget type).

### Phase B — editor v2: field bindings + preset insertion

Hook into Phase A:

1. Per-tile metric path picker. Datalist of common SignalK paths
   (`navigation.speedOverGround`, `environment.wind.angleApparent`,
   etc.) with autocomplete.
2. "Insert preset screen" button at the top of the screens area;
   opens a picker fed by `/api/presets/screens` filtered by the
   target device's display class.
3. Per-widget field config dialog matching `getWidgetMeta(type)`
   from Phase A.

### Phase C — plugin redesign for simplicity (defer until A+B are live)

1. Delete `/ui/*` handlers, replace with static HTML in `public/`.
2. Delete most `render*` functions.
3. Simplify `importDashboardPreset`.

Skip Phase C until A+B are live and the layout editor has time to bed in.

## Doing first

**Phase A** — that's what the user actually asked for. Phase B
becomes the editor v2 commit on top. Phase C is a "we have time and
no fires" cleanup that can wait a week.
