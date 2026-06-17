# Device-mirrored, version-gated layout editor with per-screen subscriptions

**Status:** Design (umbrella) · **Date:** 2026-06-17 · **Author:** navado and contributors

## Problem

The SignalK manager plugin's layout editor, device-config page, and live view
were driven by a static catalogue of nine generic preset screens, not by the
device's real screens. A recent change made the *live view* and *config page*
mirror the device's reported screens and render its built-in HUDs faithfully
(commit `e9ad755`). This spec covers the larger follow-on: make the **layout
editor** a true authoring surface for the device's *supported* views — where
each field is bound to configurable SignalK paths and presentation attributes —
and make the **firmware** subscribe only to the values a screen actually shows,
render arbitrary configured paths, and persist its applied layout across reboot.

The firmware today (see `src/signalk.cpp`, `src/layout_renderer.cpp`,
`src/ui/ui_screens.*`):

- subscribes to a **fixed global list of ~23 paths** once on WS connect; there
  is no per-screen subscribe/unsubscribe.
- renders only a **hardcoded set of ~20 recognized paths** (`path_to_source()`);
  any other configured path renders `--`.
- has **no screen on-show/on-hide hooks** — only lazy build + a 5 Hz refresh.
- gates capabilities by **board/capability only** — there is no per-firmware-
  version notion of "this device supports view types X, fonts Y, paths Z".
- applies manager-pushed config at runtime but does **not persist** the applied
  layout to flash; a reboot relies on re-fetching from the manager.

## Goals

1. **Editor authors the device's views** — CRUD of screens/views and their
   fields, each field a typed tuple bound to SignalK path(s) with configurable
   presentation (type, title, format, font size, unit, color, range/zones,
   zoom). Every option is **gated to what the connected device reports**.
2. **Firmware renders arbitrary configured paths** — a generic path→value store
   so any bound path renders, no firmware recompile per path.
3. **Per-screen subscriptions** — a screen subscribes only the paths/controls it
   shows on enter, and unsubscribes on exit (diffed so shared paths are kept).
4. **Self-describing capability manifest** — the device reports, per view type,
   the valid attributes/limits; the editor gates to that manifest.
5. **Flash-persistent applied config** — the device writes its applied layout to
   flash and restores it on boot, rendering configured screens before/without
   manager connectivity.
6. **SignalK metadata integration** — gauge/bar limits and units prefill from a
   path's SignalK `meta` when available, and configured limits can be written
   back to the field metadata (and optionally to the SignalK path `meta`).

### Non-goals

- Editing the firmware's hardcoded HUD *geometry* (autopilot/wind/wind-steer
  layouts stay firmware-owned; the editor previews them, it does not reflow
  them). The editor authors **grid/tile** screens and per-field bindings.
- Changing the manager↔device transport (still heartbeat poll + `configPush`
  push-live SignalK delta).

## Architecture

Four layers bound by one **config + manifest contract**:

```
  ┌─────────────────────────────────────────────────────────────────┐
  │  Manager (SignalK plugin, browser)                                │
  │   layout-editor CRUD ── gated by ──► capability manifest          │
  │        │ authors                                                  │
  │        ▼ field model (typed tuples)                               │
  │   profile config { widgets.items, layout.screens }                │
  └─────────┬───────────────────────────────────────────────▲────────┘
            │ config (poll + configPush)        manifest +   │ heartbeat
            ▼                                   current screen│
  ┌─────────────────────────────────────────────────────────┴────────┐
  │  Firmware                                                          │
  │   apply_config ─► FLASH-persist ─► render-by-path                  │
  │        │                                  ▲                        │
  │        ▼                                  │ reads                  │
  │   per-screen path-set ─► subscription mgr │                        │
  │        on show/hide        (diff sub/unsub)│                       │
  │                                            │                       │
  │   SignalK WS ── deltas ──► generic path→value store (PSRAM) ───────┘
  └───────────────────────────────────────────────────────────────────┘
```

- **Field model** — shared schema, the unit of configuration.
- **Capability manifest** — device → manager (heartbeat `ui.capabilities`).
- **Layout-editor CRUD** — manager, gated by the manifest.
- **Firmware runtime** — generic value store, per-screen subscription, flash
  persistence, zoom.

## Field model (core data structure)

A field/tile is a typed tuple:

```jsonc
{
  "type":   "numeric",                      // view type, gated to manifest viewTypes
  "title":  "DEPTH",
  "paths":  { "value": "environment.depth.belowTransducer" },  // named keys
  "format": "XX.X",                          // value format mask
  "size":   48,                              // font size, gated to manifest fontSizes
  "unit":   "m",                             // display unit, gated to manifest units
  "color":  { "value": "#4fc3f7" },          // named element colors; default = theme
  "range":  { "min": 0, "max": 50 },         // gauge|bar only
  "zones":  [ { "lower": 0, "upper": 5, "state": "alarm", "color": "#ff5252" } ],
  "zoomable": true,                          // numeric default true
  "zoom":   "auto"                           // "auto" | "<screenId>"
}
```

- **Named paths.** Single-value fields use `{ value }`. Multi-value fields use
  semantic keys, e.g.
  `windCircle = { value: environment.wind.speedTrue, dir: environment.wind.directionTrue }`.
  Each view type's manifest entry declares which named keys it expects
  (`numeric:{value}`, `compass:{value, dir?}`, `windCircle:{value, dir}`).
  `unit`/`format`/`size`/`range` apply to the displayed `value`.
- **Color.** A named map keyed by the view type's elements (e.g. `value`,
  `label`, `needle`, `fill`). Any unset element falls back to the **current
  theme schema** (the consolidated style config), so an unconfigured field looks
  exactly like today.
- **Range/zones.** Present for `gauge`/`bar`. Prefilled from the bound path's
  SignalK `meta` (`displayScale`, `zones`) when available; operator-overridable.
- **Zoom.** `zoomable` (any numeric `true` by default). `zoom = "auto"` scales
  the field in place on tap — numeric → bigger number, compass → bigger compass
  adapted to the value. `zoom = "<screenId>"` opens a full screen (e.g. a small
  wind compass tile with `zoom: "wind"` opens the full wind screen).

## Capability manifest (device self-report → heartbeat `ui.capabilities`)

The device reports, per view type, the valid attributes and limits, so the
editor only offers combinations the firmware can render:

```jsonc
{
  "viewTypes": {
    "numeric":    { "paths": ["value"],         "attrs": ["title","format","size","unit","color"], "zoom": ["auto"] },
    "compass":    { "paths": ["value","dir?"],   "attrs": ["title","size","color"],                 "zoom": ["auto","screenRef"] },
    "windCircle": { "paths": ["value","dir"],    "attrs": ["title","format","size","unit","color"], "zoom": ["auto","screenRef"] },
    "gauge":      { "paths": ["value"],          "attrs": ["title","size","unit","color","range","zones"], "zoom": ["auto"] },
    "bar":        { "paths": ["value"],          "attrs": ["title","size","unit","color","range","zones"], "zoom": ["auto"] },
    "trend":      { "paths": ["value"],          "attrs": ["title","size","unit","color"],          "zoom": ["auto"] },
    "text":       { "paths": ["value"],          "attrs": ["title","size","color"],                 "zoom": [] },
    "control":    { "paths": ["value"],          "attrs": ["title","size","color"],   "controls": ["autopilot"], "zoom": ["screenRef"] }
  },
  "fontSizes": [12,14,16,20,28,32,38,48,64],
  "units":     { "speed":["kn","m/s"], "angle":["deg"], "depth":["m","ft"], "temp":["C","F"], "ratio":["%"], "voltage":["V"] },
  "maxViews":   8,                      // max number of switchable screens (a "view" == a screen)
  "maxTilesPerScreen": 4,
  "paths":      "open",                 // "open" = generic store renders any path; or a curated array
  "controls":   ["autopilot"],
  "themes":     ["day","night","high-contrast"]
}
```

- `paths: "open"` tells the editor any SignalK path is bindable (the generic
  store renders it). A curated array would restrict binding to a known set.
- `maxViews` caps how many screens the editor may create (a "view" is one
  switchable screen); `maxTilesPerScreen` caps fields per screen. The editor
  blocks adding past either limit.
- The manifest is versioned implicitly by the firmware it ships with — there is
  **no manager-side version→capability table** to maintain.

## Firmware runtime

### Generic path→value store (PSRAM, ephemeral)

Replace the `path_to_source()`-only path with a PSRAM-backed map of subscribed
`path → latest value` (fixed-capacity array or open-addressed map sized to
`maxViews * maxTilesPerScreen * pathsPerField`, in PSRAM per the layout
memory rule). The renderer resolves a field's value by path string. The existing
`MetricSource` fast-path stays for built-in screens (back-compat and speed); new
authored fields go through the generic store. The store is **runtime-only** —
rebuilt from subscriptions each session, lost on reboot, repopulated on connect.

### Per-screen path-set + subscription manager

- Each screen exposes `collect_paths()` → the set of paths it shows, including
  control paths. Built-in screens derive this from their `MetricBinding` tables
  (a `MetricSource → path` reverse map); authored screens read it from the
  layout's field `paths`.
- Add **on-show / on-hide** hooks to the screen registry (`src/ui/ui_screens.*`,
  `screen_manager.cpp`). On show: compute the new screen's path-set, send a
  SignalK `subscribe` for paths not already active and an `unsubscribe` for
  active paths the new screen does not need (diff; never drop a path the new
  screen also needs). A small **always-on baseline** stays subscribed
  (`network.espdisp.configPush`, autopilot state if AP enabled, anything the
  manager needs for liveness).
- This bounds the subscription set to the active screen, cutting WS traffic and
  parser load.

### Flash-persistent applied config

- On successful `apply_config`, write the applied layout/widget config blob to
  flash (NVS namespace or LittleFS file), with the config **hash/version** so a
  reboot reloads it without a re-fetch.
- Boot flow: load persisted config from flash → render screens → connect SignalK
  → subscribe the active screen's paths → fill the PSRAM value store. The device
  shows its configured layout immediately after reboot, even with the manager
  offline.
- When the manager reconnects, the device compares its persisted config hash to
  the manager's desired hash (existing drift mechanism) and fetches only on
  change.

### Zoom

A tap on a zoomable field either scales it in place (`auto`) or issues a `view`
switch to the referenced screen (`screenRef`), reusing the existing screen-show
path (which now also drives the subscription diff).

## Manager layout editor (CRUD)

- Reads the connected device's `ui.capabilities` manifest and renders a CRUD
  editor for **that device's** views: add/rename/reorder/delete screens (capped
  at `maxViews`), add/remove/configure fields per screen (capped at
  `maxTilesPerScreen`).
- Per field, the editor offers only manifest-valid choices: view type, named
  path binding(s) (autocomplete from the SignalK path tree; `paths:"open"`
  allows any), title, format mask, font size (from `fontSizes`), unit (from the
  unit family for the path's quantity), color, and — for gauge/bar — range and
  zones.
- **Color** — a color-scheme preset selector (Day/Night/High-contrast palettes)
  sets element colors from the chosen scheme; an inline color-preset picker
  (theme named swatches + custom) overrides per element. Unset = theme default.
- **SignalK metadata** — when a path is bound, the editor fetches its SignalK
  `meta` (unit, `displayScale.lower/upper`, `zones`) and prefills the field's
  unit and gauge/bar range/zones. The operator can override; a **"save limits"**
  action persists the configured range/zones onto the field metadata and,
  optionally, writes them back to the SignalK path `meta` so the source and
  other clients share them.
- Persists into the profile config (`widgets.items` + `layout.screens`) and
  pushes via the existing poll + `configPush` path. Built-in fullscreen HUDs
  remain non-editable previews (rendered by `device-hud.js`).

## Config / protocol contract

- **Heartbeat** gains `ui.capabilities` (the manifest above), alongside the
  existing `ui.screens` / `ui.screen` already reported.
- **Config schema** (`config.widgets.items[*]`, `config.layout.screens[*].tiles[*]`)
  is extended with: named `paths`, `format`, `size`, `unit`, `color`, `range`,
  `zones`, `zoomable`, `zoom`. Existing single-`path` widgets remain valid
  (mapped to `paths.value`).
- The device validates an incoming config against its own manifest and rejects
  (with a `ParseError`) anything it cannot honor, falling back to the last
  persisted config.

## Implementation sequencing

Each slice gets its own implementation plan and lands independently behind the
contract.

1. **Firmware generic path store + render-by-path.** PSRAM value map; renderer
   resolves authored fields by path string; `MetricSource` fast-path retained.
   *Foundation; invisible until the editor uses it.*
2. **Capability manifest reporting + manager ingest.** Device emits
   `ui.capabilities`; manager parses and exposes it; editor reads it (no UI
   gating yet).
3. **Screen lifecycle hooks + subscription manager.** on-show/on-hide;
   `collect_paths()` (incl. `MetricSource → path` reverse map); subscribe/
   unsubscribe diff; always-on baseline.
4. **Flash-persistent applied config.** Write-on-apply, load-on-boot, hash/
   version, drift check on reconnect.
5. **Manager editor CRUD + field schema.** Typed-tuple fields; manifest-gated
   type/path/format/size/unit/color/range/zones; color-scheme presets + inline
   picker; SK-metadata prefill + save-back; view/tile-count limits.
6. **Zoom semantics.** `zoomable` + `zoom` (`auto` in-place scale; `screenRef`
   view switch via the subscription-aware show path).

## Testing

- **Firmware (native/Unity, host):** generic path-store get/set; `collect_paths()`
  for built-in and authored screens; subscription-diff (subscribe/unsubscribe
  sets) as a pure function; config parse/validate against manifest; flash
  persist/restore round-trip (mocked storage). Sim renders for each field view
  type, color, range/zones, and zoom.
- **Manager (node `test/run.js`):** manifest parse + editor gating (invalid
  type/font/unit rejected); field-schema CRUD + config generation; SK-metadata
  prefill + save-back; view/tile limit enforcement; back-compat of legacy
  single-`path` widgets.
- **Contract:** a shared fixture of the extended config schema exercised on both
  sides so the manager never emits a config the device would reject.

## Risks / mitigations

- **Memory.** Generic store + per-screen path-sets must live in PSRAM, never on
  task stacks (the project's large-struct-on-stack reboot trap). Size to the
  manifest caps; allocate with `heap_caps_*` SPIRAM.
- **Subscription churn.** Rapid screen switching could thrash subscribe/
  unsubscribe; debounce and diff so shared paths are never dropped/re-added.
- **Config schema drift.** The device must validate against its own manifest and
  fall back to the persisted config on reject, so a newer manager cannot brick
  an older device's display.
- **Flash wear.** Persist only on *changed* applied config (hash compare), not
  every heartbeat.
