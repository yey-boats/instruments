# YB-MIDL Firmware Render Port — Slice 2 (Render Core) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Render a resolved MIDL document on the device by turning a solved `midl::PlacementSet` (Slice 1) + per-element specs into an LVGL screen, **reusing the existing `WidgetKind` painters** — as a **new, flag-gated, parallel path on a dedicated screen** that leaves every shipped screen byte-identical.

**Architecture:** Three pieces. (1) A pure, host-tested mapper `midl::render::map_element()` turns one MIDL element JSON (`JsonVariantConst`) into a `ui::layouts::MetricBinding` via the compiled `MetricSource` enum bridge (`path_to_source`) — unknown paths render `--`, exactly like the legacy editor path. (2) An **additive** builder `ui::layouts::create_freeform()` in `ui/ui_layouts.cpp` places one tile per `Placement` at the solver's pixel rect, reusing the existing `static` painters via the existing per-tile build path — no existing template or painter is modified. (3) `src/midl_render.cpp` orchestrates: parse doc → `midl::solve_screen` → map each placement's element → build → register a screen + refresh trampoline, driven on the UI task through a new `CommandType::ConfigApplyMidl` app-event and a `midl-render` console command that loads a baked test doc.

**Tech Stack:** C++17, PlatformIO `native` (Unity host tests) + `esp32-4848s040` (device) + `make sim` (headless LVGL), ArduinoJson v7, LVGL 9. Reuses landed `midl_solve.*`, `widget_data_resolver.*`, `sk::PathStore`, `ui::layouts` painters, `app_events`.

This is **Slice 2 of `docs/superpowers/plans/2026-06-21-yb-midl-firmware-render-port.md`**. Depends on Slice 1 (landed: `include/midl_solve.h`, `src/midl_solve.cpp`). **Binding is the `MetricSource` enum bridge** (brainstorm: generic-path store is a later refinement). **Delivery (POST/SignalK) and persistence are NOT in this slice** — Slice 2 loads a baked test doc; real delivery is Slice 4.

---

## Context the implementer needs (verified by recon)

- **Painters** (`src/ui/ui_layouts.cpp`): `static void paint_<kind>_body(QuadGridTile &t, const MetricBinding &m, int w, int h)` for numeric/compass/gauge/bar/wind_rose/text/button/autopilot. They take an arbitrary pixel `(w,h)` — geometry is NOT QuadGrid-locked. The per-tile builder `build_tile(...)` (line ~909) creates the tile container + dispatches to the painter via `switch (m.kind)`. These are `static`, so the freeform builder must live **in the same TU** (`ui_layouts.cpp`) to call them.
- **`MetricBinding`** (`include/ui_layouts.h:93`): `{ const char *id; const char *label; const char *unit; MetricSource source; uint32_t accent; const char *target_screen; uint8_t extras_count; MetricRow extras[4]; WidgetKind kind; bool zoomable; const char *zoom_target; }`. Strings are non-owning `const char*` — they must outlive the screen (point into a session-lived store, never a stack temp).
- **`MetricSource`** enum (`include/ui_layouts.h:45`) — 24 compiled values; `None` renders `--`.
- **`ScreenVariantSpec`** (`include/ui_layouts.h:118`): `{ const char *screen_id; const char *title; TemplateId template_id; const MetricBinding *metrics; uint8_t metric_count; uint8_t variant_flags; }`.
- **Existing mappers to REUSE** (`src/layout_renderer.cpp` / `include/layout_renderer.h`): `ui::layout_render::widget_to_kind(const char*)` maps editor strings → `WidgetKind`; `path_to_source(const char*)` maps a SK path → `MetricSource`. NOTE: `widget_to_kind` expects editor tokens (`"windRose"`, `"numeric"`); **MIDL uses different tokens** (`"single-value"`, `"windrose"`), so Slice 2 adds a MIDL-token map (Task 1) rather than reusing `widget_to_kind` directly.
- **Screen registration** (`include/ui_screens.h`): `register_screen_lazy(id,title,build_fn,refresh_fn,hidden)` and `replace_screen(id, Screen)`; `Screen{ id,title,root,refresh,hidden,build_fn,collect_paths,build_us }`. Legacy analog: `src/layout_renderer.cpp:145-233` builds a `ScreenVariantSpec`, calls `ui::layouts::create(nullptr, spec)`, fills a `Screen`, calls `ui::replace_screen`. Mirror that flow.
- **app_events** (`include/app_events.h`): `enum class CommandType` + `Command{ type; char a[256]; char b[256]; int32_t i; void* blob; size_t blob_len; uint32_t t_post_us; }`. `app::post(cmd)` → UI queue; `pump()` drains on the LVGL task (`src/app_events.cpp:147`), frees `cmd.blob` after handling (line ~276). `ApplyLayout` (line 192) is the exact template: blob = JSON bytes, handler runs on UI task, builds LVGL. Add `ConfigApplyMidl` the same way.
- **Binding resolution** (`include/widget_data_resolver.h`): `double widget_data::resolve_numeric(const char *path, const sk::Data&)` and an overload taking `const sk::PathStore*`. The painters' update path resolves via `format_metric()` switching on `MetricSource` — so on the enum bridge, values flow through the existing update with **no painter change**.
- **Memory traps (CLAUDE.md):** LVGL only on the UI task (guaranteed inside `pump()`); per-tile/screen state structs go on the heap (`heap_caps_calloc`), never large stack locals; the doc blob is PSRAM. `MetricBinding[]` + its backing strings must be session-lived (heap/PSRAM, not stack) because they're stored by pointer.

---

## File Structure

- `include/midl_render.h` (create) — public API: `midl::render::map_element()` (pure) + `midl::render::apply_doc()` (UI-task entry) + `midl::render::handle_command()`.
- `src/midl_render.cpp` (create) — orchestration: parse → solve → map → build → register; owns a session-lived arena for `MetricBinding[]` + strings.
- `include/ui_layouts.h` (modify) — declare `lv_obj_t *create_freeform(lv_obj_t *parent, const ScreenVariantSpec &spec, const Rect *rects);` (additive).
- `src/ui/ui_layouts.cpp` (modify, additive only) — implement `create_freeform` + a matching `update_freeform`; reuse `build_tile`/painters. Do NOT touch existing templates/painters.
- `include/app_events.h` + `src/app_events.cpp` (modify) — add `CommandType::ConfigApplyMidl` + its `pump()` case.
- `src/main.cpp` or the right console handler (modify) — `midl-render [screenId]` console command that loads a baked test doc and posts `ConfigApplyMidl`.
- `test/test_midl_render/test_midl_render.cpp` (create) — host tests for `map_element` (pure).
- `platformio.ini` (modify) — register `midl_render.cpp` (native `build_src_filter`) + `test_midl_render` (native `test_filter`).

---

## Task 1: Pure element → MetricBinding mapper (host-tested)

**Files:** Create `include/midl_render.h`, `src/midl_render.cpp` (mapper only), `test/test_midl_render/test_midl_render.cpp`; modify `platformio.ini`.

- [ ] **Step 1: Define the mapper API**

In `include/midl_render.h`:

```cpp
#pragma once
#include <ArduinoJson.h>
#include "ui_layouts.h"   // ui::layouts::MetricBinding, WidgetKind, MetricSource

namespace midl { namespace render {

// Map a MIDL element token ("single-value","text","gauge","bar","compass",
// "windrose","trend","autopilot","button") to a device WidgetKind. Unknown
// tokens fall back to Numeric (matches the legacy renderer's policy).
ui::layouts::WidgetKind token_to_kind(const char *type);

// Pure: fill `out` from one MIDL element JSON. Strings (id,label,unit) are
// COPIED into caller-owned buffers `id_buf`/`label_buf`/`unit_buf` (each >=32),
// and out.id/label/unit point at them — so the caller controls lifetime (the
// MetricBinding stores non-owning pointers). `value` binding path -> source via
// path_to_source; style.color -> accent. Returns false if `el` is not an object.
bool map_element(JsonVariantConst el, const char *element_id,
                 ui::layouts::MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf);

}}  // namespace midl::render
```

- [ ] **Step 2: Write the failing test**

Create `test/test_midl_render/test_midl_render.cpp`:

```cpp
#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "midl_render.h"

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

static MetricBinding mapOne(const char *json, const char *id) {
    JsonDocument doc; deserializeJson(doc, json);
    static char idb[32], lab[32], unit[32];
    MetricBinding mb{};
    midl::render::map_element(doc.as<JsonVariantConst>(), id, mb, idb, lab, unit);
    return mb;
}

void test_token_to_kind() {
    TEST_ASSERT_EQUAL(WidgetKind::Numeric,  midl::render::token_to_kind("single-value"));
    TEST_ASSERT_EQUAL(WidgetKind::WindRose, midl::render::token_to_kind("windrose"));
    TEST_ASSERT_EQUAL(WidgetKind::Compass,  midl::render::token_to_kind("compass"));
    TEST_ASSERT_EQUAL(WidgetKind::Gauge,    midl::render::token_to_kind("gauge"));
    TEST_ASSERT_EQUAL(WidgetKind::Numeric,  midl::render::token_to_kind("frobnicate"));
}

void test_map_single_value() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, m.kind);
    TEST_ASSERT_EQUAL_STRING("SOG", m.label);
    TEST_ASSERT_EQUAL_STRING("kn", m.unit);
    TEST_ASSERT_EQUAL_STRING("sog", m.id);
    TEST_ASSERT_EQUAL(MetricSource::SOG_kn, m.source);
}

void test_map_unknown_path_is_none() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"made.up.path"}}})",
        "x");
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);  // renders "--"
}

void test_map_compass_label_fallback_to_id() {
    MetricBinding m = mapOne(
        R"({"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(WidgetKind::Compass, m.kind);
    TEST_ASSERT_EQUAL_STRING("hdg", m.label);  // no name -> label falls back to id
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_token_to_kind);
    RUN_TEST(test_map_single_value);
    RUN_TEST(test_map_unknown_path_is_none);
    RUN_TEST(test_map_compass_label_fallback_to_id);
    return UNITY_END();
}
```

- [ ] **Step 3: Register in the native env**

In `platformio.ini` `[env:native]`: add `midl_render.cpp` to `build_src_filter` and `test_midl_render` to `test_filter` (append; keep existing). Note `midl_render.cpp` includes `ui_layouts.h` — confirm `ui_layouts.h` compiles host-side; if it pulls LVGL headers that don't build native, **split the enum/struct decls** the mapper needs are already host-clean (MetricBinding/MetricSource/WidgetKind are POD enums/structs with no LVGL types). If `ui_layouts.h` is not host-compilable, the mapper must include only the POD portion — in that case create `include/ui_layouts_types.h` with the enums/`MetricBinding`/`MetricRow`/`ScreenVariantSpec`/`MetricSource` and have `ui_layouts.h` include it; map_element includes only the types header. Verify which is needed and do the minimal split. **Report as DONE_WITH_CONCERNS if a header split was required.**

- [ ] **Step 4: Run to verify it fails**

Run: `pio test -e native -f test_midl_render` — expect link/compile failure (mapper unimplemented).

- [ ] **Step 5: Implement the mapper**

In `src/midl_render.cpp` (mapper portion). Reuse `ui::layout_render::path_to_source` (declared in `include/layout_renderer.h` — include it). Implement `token_to_kind` for the MIDL tokens, copy `name` (or `element_id` fallback) → label, `format.unit` → unit, `bindings.value.path` → `path_to_source`, `style.color` (hex string or int) → accent:

```cpp
#include "midl_render.h"
#include "layout_renderer.h"   // ui::layout_render::path_to_source
#include <string.h>

namespace midl { namespace render {

using ui::layouts::MetricBinding;
using ui::layouts::WidgetKind;

WidgetKind token_to_kind(const char *t) {
    if (!t) return WidgetKind::Numeric;
    if (!strcmp(t, "compass")) return WidgetKind::Compass;
    if (!strcmp(t, "windrose")) return WidgetKind::WindRose;
    if (!strcmp(t, "gauge")) return WidgetKind::Gauge;
    if (!strcmp(t, "bar")) return WidgetKind::Bar;
    if (!strcmp(t, "autopilot")) return WidgetKind::Autopilot;
    if (!strcmp(t, "text")) return WidgetKind::Text;
    if (!strcmp(t, "button")) return WidgetKind::Button;
    if (!strcmp(t, "trend")) return WidgetKind::Trend;
    return WidgetKind::Numeric;  // "single-value" + unknown
}

static void copy32(char *dst, const char *src) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, 31); dst[31] = 0;
}

bool map_element(JsonVariantConst el, const char *element_id, MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf) {
    if (!el.is<JsonObjectConst>()) return false;
    memset(&out, 0, sizeof(out));
    copy32(id_buf, element_id);
    const char *name = el["name"] | element_id;
    copy32(label_buf, name);
    copy32(unit_buf, el["format"]["unit"] | "");
    out.id = id_buf; out.label = label_buf; out.unit = unit_buf;
    out.kind = token_to_kind(el["type"] | "");
    out.source = ui::layout_render::path_to_source(el["bindings"]["value"]["path"] | "");
    // style.color may be "#rrggbb" or an int; default 0 (painter uses theme default).
    out.accent = 0;
    JsonVariantConst col = el["style"]["color"];
    if (col.is<unsigned>()) out.accent = col.as<unsigned>();
    return true;
}

}}  // namespace midl::render
```

- [ ] **Step 6: Run to verify pass**

Run: `pio test -e native -f test_midl_render` — expect 4/4 PASS. Then `pio test -e native` (full suite) — expect all green (no regressions).

- [ ] **Step 7: Commit**

```bash
git add include/midl_render.h src/midl_render.cpp test/test_midl_render/ platformio.ini
git commit -m "feat(midl): pure MIDL-element -> MetricBinding mapper (enum bridge, host-tested)"
```
(Trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Do not push until the slice is reviewed.)

---

## Task 2: Additive freeform builder in ui_layouts (device/sim)

**Files:** modify `include/ui_layouts.h` (declare), `src/ui/ui_layouts.cpp` (additive impl).

> This task adds a NEW builder beside the existing templates and **must not modify any existing painter or template function**. Verify with `git diff` that only additions were made to existing functions' surroundings.

- [ ] **Step 1: Declare the builder + its update**

In `include/ui_layouts.h` (near `create`/`update` decls), add:

```cpp
// Freeform layout: one tile per (MetricBinding, Rect) pair, placed at the
// caller's solved pixel rect. Reuses the per-tile painters. `spec.metrics`
// has `spec.metric_count` entries; `rects[i]` is the pixel rect for metric i.
lv_obj_t *create_freeform(lv_obj_t *parent, const ScreenVariantSpec &spec, const Rect *rects);
void update_freeform(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data);
```

Add a `struct Rect { int x, y, w, h; };` to `ui_layouts.h` if one is not already visible there (it must match `midl::Rect`'s field order; the caller converts). If a name clash with `midl::Rect` is a concern, accept `const int (*rects)[4]` or a small local POD — implementer's discretion, but keep it POD and documented.

- [ ] **Step 2: Implement create_freeform/update_freeform**

In `src/ui/ui_layouts.cpp`, after the existing templates (additive). Model on `create_quad_grid` but place each tile at `rects[i]` instead of computing a 2×2 grid; reuse the existing per-tile build path (`build_tile` / `style_panel` / painter dispatch) and the existing per-tile state array pattern (heap-allocated `*State` attached via `lv_obj_set_user_data`). `update_freeform` mirrors the existing `update` per-tile loop. **Reuse, do not reimplement, the painter bodies.** Keep all scratch on the heap (`heap_caps_calloc`), nothing large on the stack. Register `TemplateId` is not required — `create_freeform` is called directly by midl_render (not via `create()`), so no enum churn.

- [ ] **Step 3: Build for device**

Run: `pio run -e esp32-4848s040` — expect a clean build (this is the first device-compiled MIDL code; watch for the memory-trap patterns).

- [ ] **Step 4: Commit**

```bash
git add include/ui_layouts.h src/ui/ui_layouts.cpp
git commit -m "feat(midl): additive freeform LVGL builder reusing tile painters"
```

---

## Task 3: midl_render orchestration + screen registration

**Files:** modify `include/midl_render.h`, `src/midl_render.cpp`.

- [ ] **Step 1: Add the apply entry**

`midl::render::apply_doc(JsonVariantConst doc, const char *screen_id)` (runs on UI task): find the screen object in `doc["screens"]` (by id, or the first); `midl::solve_screen(screen["layout"], {0,0,480,480}, placements)`; for each placement, look up `screen["elements"][placement.element]`, call `map_element` into a **session-lived arena** (a `static` or PSRAM-held array of `MetricBinding` + `char[32]` triples sized to `FirmwareLimits::max_tiles_per_screen`); build a `ScreenVariantSpec` + `Rect[]` from placements; `lv_obj_t *root = ui::layouts::create_freeform(nullptr, spec, rects)`; fill a `Screen{}` with a refresh trampoline calling `update_freeform`; `ui::replace_screen(screen_id, scr)` (register lazily first if absent). Mirror `src/layout_renderer.cpp:145-233`. The arena must be **function-static or PSRAM**, never stack (MetricBinding stores pointers; recon §8 + CLAUDE.md). Single-screen for Slice 2 (multi-screen is Slice 5/cutover) — document that.

- [ ] **Step 2: Build for device + commit**

Run `pio run -e esp32-4848s040`. Commit `feat(midl): render orchestration (doc -> solve -> map -> freeform screen)`.

---

## Task 4: ConfigApplyMidl app-event + `midl-render` console command

**Files:** modify `include/app_events.h`, `src/app_events.cpp`, the console handler + a baked test doc.

- [ ] **Step 1: Add the command type + pump case**

Add `ConfigApplyMidl` to `CommandType`. In `pump()`, add a case mirroring `ApplyLayout` (line 192): parse the blob JSON into a PSRAM `JsonDocument`, call `midl::render::apply_doc(doc, a[0]?a:"midl")`, log result. The blob is freed by the existing post-switch `heap_caps_free` — do not double-free.

- [ ] **Step 2: Add the console command**

`midl-render [screenId]`: load a baked MIDL test doc (embed one projection fixture, e.g. `steering-4tile`, as a `const char*` in a new `include/midl_demo_doc.h`, OR read it from the existing layout transport), copy to a PSRAM blob, post `ConfigApplyMidl`. Register it in the right handler (`handleMainCommand` in `main.cpp` per CLAUDE.md "A new console command") so it reaches serial + BLE via `net::dispatchCommand`. Add it to `cmd_catalog.cpp` if other UI commands are listed there.

- [ ] **Step 3: Build + commit**

`pio run -e esp32-4848s040`. Commit `feat(midl): ConfigApplyMidl app-event + midl-render console command (baked test doc)`.

---

## Task 5: Verify render — sim + device

**Files:** none (verification) — may add a sim entry if needed.

- [ ] **Step 1: Headless sim render**

If `make sim` can target a MIDL doc, render the `steering-4tile` projection and assert no tile overlap / in-bounds at 480×480 (reuse the sim's existing no-overlap check). If the sim harness can't yet load a MIDL doc, add a minimal sim entry that calls `midl::render::apply_doc` against the baked doc and snapshots. Capture the snapshot.

- [ ] **Step 2: On-device verify**

Flash (`make ota DEVICE_IP=<ip>` or `make flash`), run `midl-render steering`, confirm the screen renders the 4 tiles at the solved rects without a reboot. Pull `/api/screenshot.png` to confirm. If it reboots, this is the memory-trap path — capture the boot log and fix before claiming done (CLAUDE.md silent-reboot signature).

- [ ] **Step 3: Full host suite + push**

`pio test -e native` (all green), then `git push origin feat/midl-firmware-render`.

---

## Slice 2 Self-Review

- **No shipped-screen change:** Tasks 2's diff is additive (new functions only); legacy templates/painters untouched — verify via `git diff`. The MIDL path is reachable only via the `midl-render` command / flag.
- **Enum bridge (not generic path):** Task 1 maps via `path_to_source`; unknown paths → `None` → `--`. Generic-path binding is a later refinement (noted, not dropped).
- **Memory traps:** doc blob in PSRAM; `MetricBinding[]`+strings in a session-lived arena (not stack); tile state heap-allocated; all LVGL inside `pump()` on the UI task.
- **Deferred to later slices:** delivery (POST/SignalK) + validation = Slice 4; persistence/rollback = Slice 3; multi-screen + legacy cutover = Slice 5.
