# YB-MIDL Firmware Render Port (Plan 6) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **This plan fully specifies Slice 1.** Slices 2–5 are scoped at the end and detailed just-in-time after each prior slice lands and is reviewed (same pattern as `2026-06-19-yb-midl-manager-migration.md`).

**Goal:** Make the firmware render **resolved single-class MIDL documents** natively — replacing the fixed `TemplateId`/QuadGrid path with a generic recursive split/grid solver feeding the existing element painters — with flash-persisted rollback tiers and an A/B-safe apply.

**Architecture:** Three layers (per spec §4 and the brainstorm). (1) A pure-C++ **integer layout solver** walks the MIDL `layout` node tree and assigns a pixel rect to each leaf — a depth-bounded port of `midl/web/src/solve.ts`, host-tested against shared fixtures. (2) The existing 9 `WidgetKind` **painters** are made parent-agnostic so they paint into any solved rect, with bindings resolved through the **already-landed `sk::PathStore`** (commit `58621ec`). (3) **Chrome** (MOB pill, FPS, nav) stays on `lv_layer_top()`, untouched. Apply runs on the UI task via the existing `app_events` queue seam; the doc is persisted to **LittleFS** with current/last-known-good/factory tiers and a boot-loop counter in NVS.

**Tech Stack:** C++17, PlatformIO `native` (Unity host tests) + `esp32-4848s040` (device), ArduinoJson v7 (PSRAM-allocated docs), LVGL 9, ESP-IDF `heap_caps` + LittleFS + NVS (`storage::Namespace`).

This is **Plan 6 of the YB-MIDL runtime spec** (`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md` §5 item 6). It depends on **Plan 5** (projection fixtures + `include/midl_limits.h` + locked class limits) and on Slice 1 of the **manager-migration** plan for end-to-end delivery, but Slice 1 below stands alone (host-tested, no device dependency).

---

## Reconciliation notes (read before starting — these override the reviewed spec where flagged)

Per the user's latest decisions (recorded 2026-06-21), three points differ from the reviewed spec and are pinned to the slices that implement them:

1. **Action gating — OUT OF SCOPE (Slice 4).** Spec §3.4 specifies put/command allowlists, auth levels, and confirmation. The device is treated as a trusted control+display tool with intentionally weak auth; **no gating is implemented.** Slice 4 wires action *execution* (nav/put/command via `net::dispatchCommand`) only.
2. **On-device validation = render-safety, not security (Slice 4).** Spec §3.6 makes the device "defensive parse only." Because the device web UI also serves the editor and posts directly (no manager guaranteed in the loop), the device runs enough validation to **never crash-render** — structural + capability (vs the embedded manifest) + geometry (min-rect / no-overlap / in-bounds / depth-limit). This is reliability, not access control.
3. **Storage = LittleFS (Slice 3).** Spec leans on the in-RAM `s_last_json`; the multi-KB doc is persisted to a **LittleFS** partition instead, with the small boot-loop counter + "pending" flag in **NVS** (`storage::Namespace`). `s_last_json` remains the in-RAM last-good mirror.

The `dir` vs `flow` split-key question (spec EBNF says `dir`; MIDL submodule schema says `flow`, commit `83ed4e0`) is resolved in **Slice 1 Task 1**: the firmware accepts **both** keys, preferring `flow`. Plan 5's fixtures use `dir`; they are migrated to `flow` in Slice 1 Task 1's commit if `flow` is chosen as canonical.

---

## Context an out-of-context engineer needs

- **The web solver to port:** `midl/web/src/solve.ts` (50 lines). Leaf → rect; split divides along `dir`/`flow` by `weights` (default equal); grid divides `rows × cols` row-major. It uses **floats**; the firmware spec §3.3 mandates **integer** geometry with deterministic remainder distribution so web and LVGL agree pixel-for-pixel. The port therefore uses integer boundary math (see Slice 1 Task 2), not float multiply.
- **The painters already exist:** `src/ui/ui_layouts.cpp` has `WidgetKind` painters (Numeric/Text/Gauge/Bar/Compass/WindRose/Trend/Autopilot/Button) but they are invoked only inside fixed templates (`QuadGrid`, `split_pair`, …). `include/layout_renderer.h` already maps a widget string → `WidgetKind` (`widget_to_kind`) and a path → `MetricSource` (`path_to_source`). Slice 2 generalises these to paint into solver rects with `PathStore`-resolved bindings.
- **Binding resolution is done:** `sk::PathStore` (`include/path_store.h`, `src/path_store.cpp`) + `widget_data::resolve_numeric()` resolve an arbitrary SignalK path string → latest value. The renderer consumes it; no new data plumbing is needed for arbitrary paths.
- **The apply seam:** `src/app_events.cpp` runs a UI queue and a net queue. A `ConfigApplyLayout` command already carries a layout JSON PSRAM blob from the web/net task to the UI task (`src/app_events.cpp:202` → `ui::layout_render::apply()`). Slice 2 adds a `ConfigApplyMidl` command on the same seam. **All `lv_obj_*` work runs on the UI task** ([[feedback-lvgl-only-on-ui-task]]).
- **Memory traps (CLAUDE.md):** never stack-build a big struct (use PSRAM/`static`+`memset`); `layout::parse` uses `memset`, not `out = Config{}`; the live config is PSRAM-allocated; recursion must bound depth *as it descends*. The solver frames are tiny (a `Rect` + a couple ints) and depth ≤ `midl::MAX_LAYOUT_DEPTH = 3`, so they are stack-safe — but the input `JsonDocument` is PSRAM-allocated on device.
- **Native test convention:** `test/test_<name>/`, registered in `platformio.ini` native `build_src_filter` + `test_filter`. The native env builds only pure-C++ TUs.

---

## File Structure (Slice 1)

- `include/midl_solve.h` (create) — `midl::Rect`, `midl::Placement`, fixed-capacity `midl::PlacementSet`, and `midl::solve_screen()` / `midl::expand_preset()`. One responsibility: geometry. Pure C++, no Arduino/LVGL deps beyond ArduinoJson.
- `src/midl_solve.cpp` (create) — the integer solver + preset expander + depth/capacity guards.
- `test/test_midl_solve/test_midl_solve.cpp` (create) — Unity host tests: split/grid integer math, preset expansion, depth/capacity rejection, parity rects for the Plan 5 projection fixtures.
- `platformio.ini` (modify) — add `midl_solve.cpp` to native `build_src_filter`; add `test_midl_solve` to `test_filter`.

(Slices 2–5 add their own files — see the slice map.)

---

## Slice 1 — Integer layout solver (pure, host-tested)

### Task 1: Geometry types + solver header

**Files:**
- Create: `include/midl_solve.h`
- Modify: `platformio.ini` (native `build_src_filter` + `test_filter`)

- [ ] **Step 1: Create the header**

Create `include/midl_solve.h`:

```cpp
#pragma once

// Pure-C++ integer layout solver for resolved single-class MIDL documents.
// Walks a layout node tree (leaf | split{dir|flow,children,weights} |
// grid{rows,cols,cells} | preset) and assigns an integer pixel rect to each
// leaf. Deterministic remainder distribution so this matches the web/Canvas
// renderer pixel-for-pixel (spec §3.3 "Layout solving contract").
//
// Host-testable: depends only on ArduinoJson. The device feeds it a
// PSRAM-allocated JsonDocument; recursion is bounded by midl::MAX_LAYOUT_DEPTH
// so solver frames are stack-safe.

#include <ArduinoJson.h>
#include <stddef.h>

#include "midl_limits.h"  // midl::MAX_LAYOUT_DEPTH, midl::FirmwareLimits

namespace midl {

struct Rect { int x, y, w, h; };

struct Placement {
    char element[FirmwareLimits::str_len];  // element id (key in screen.elements)
    Rect rect;
};

// Fixed-capacity output (one screen). No dynamic allocation.
struct PlacementSet {
    Placement items[FirmwareLimits::max_tiles_per_screen];
    size_t count = 0;
};

enum SolveStatus {
    SOLVE_OK = 0,
    SOLVE_TOO_DEEP,        // nesting exceeded MAX_LAYOUT_DEPTH
    SOLVE_TOO_MANY_TILES,  // more leaves than max_tiles_per_screen
    SOLVE_BAD_NODE,        // a node matched none of leaf/split/grid/preset
    SOLVE_UNKNOWN_PRESET,  // preset name not in the built-in set
};

// Expand a preset node `{preset, slots}` into concrete geometry by solving it
// directly (the firmware never materialises an intermediate tree). Known
// presets: "full" (1 slot) and "hero-split" (3 slots, {1,{2,3}}).
SolveStatus expand_preset(JsonVariantConst node, Rect area, PlacementSet &out);

// Solve one screen's `layout` node into leaf placements within `area`.
// `area` is the content rect for the class (full-frame in Slice 1; safe-area
// + gutter/padding arrive in Slice 2). Returns SOLVE_OK and fills `out`, or a
// failure code and leaves `out` partially filled (caller discards on failure).
SolveStatus solve_screen(JsonVariantConst layout, Rect area, PlacementSet &out);

}  // namespace midl
```

- [ ] **Step 2: Register the TU + test in the native env**

In `platformio.ini`, under `[env:native]`:
- add `midl_solve.cpp` to `build_src_filter` (keep `signalk_parser.cpp`, `layout.cpp`, `path_store.cpp`, and any others already listed);
- add `test_midl_solve` to `test_filter`.

Example (merge with existing entries, do not drop them):

```ini
build_src_filter = +<signalk_parser.cpp> +<layout.cpp> +<path_store.cpp> +<midl_solve.cpp> +<widget_data_resolver.cpp>
test_filter = test_parser, test_layout, test_path_store, test_midl_projection, test_midl_solve
```

- [ ] **Step 3: Commit**

```bash
git add include/midl_solve.h platformio.ini
git commit -m "feat(midl): solver geometry types + header (firmware render port)"
```

> **`dir`/`flow` decision:** the solver (Task 2) reads **`flow` first, then `dir`** as a fallback, so both spellings work. `flow` is canonical. In this commit, also migrate the Plan 5 fixtures (`test/fixtures/yb-midl/projection/*.json`) from `dir` to `flow` and re-run `pio test -e native -f test_midl_projection` to confirm they still pass.

---

### Task 2: Split + leaf solving (integer, remainder-distributed)

**Files:**
- Create: `src/midl_solve.cpp`
- Test: `test/test_midl_solve/test_midl_solve.cpp`

- [ ] **Step 1: Write the failing test**

Create `test/test_midl_solve/test_midl_solve.cpp`:

```cpp
#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>

#include "midl_solve.h"

using midl::PlacementSet;
using midl::Rect;

static PlacementSet solve(const char *json, Rect area) {
    JsonDocument doc;
    deserializeJson(doc, json);
    PlacementSet out;
    midl::solve_screen(doc.as<JsonVariantConst>(), area, out);
    return out;
}

static const midl::Placement *find(const PlacementSet &p, const char *id) {
    for (size_t i = 0; i < p.count; i++)
        if (strcmp(p.items[i].element, id) == 0) return &p.items[i];
    return nullptr;
}

void test_leaf_fills_area() {
    PlacementSet p = solve(R"({"element":"a"})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(480, a->rect.w);
    TEST_ASSERT_EQUAL_INT(480, a->rect.h);
}

void test_row_split_equal() {
    PlacementSet p = solve(R"({"flow":"row","children":[{"element":"a"},{"element":"b"}]})",
                           {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.w);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.w);
}

void test_row_split_weighted_distributes_remainder() {
    // 481 px, weights 1:2 -> boundaries at floor(481*1/3)=160, then 481.
    // a = [0,160), b = [160,481): widths 160 and 321, summing to 481 exactly.
    PlacementSet p = solve(R"({"flow":"row","weights":[1,2],"children":[{"element":"a"},{"element":"b"}]})",
                           {0, 0, 481, 480});
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.x);
    TEST_ASSERT_EQUAL_INT(160, find(p, "a")->rect.w);
    TEST_ASSERT_EQUAL_INT(160, find(p, "b")->rect.x);
    TEST_ASSERT_EQUAL_INT(321, find(p, "b")->rect.w);
}

void test_col_split_nested() {
    PlacementSet p = solve(
        R"({"flow":"col","children":[{"element":"a"},{"flow":"row","children":[{"element":"b"},{"element":"c"}]}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.h);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.w);  // half of 480 width
    TEST_ASSERT_EQUAL_INT(240, find(p, "c")->rect.x);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_leaf_fills_area);
    RUN_TEST(test_row_split_equal);
    RUN_TEST(test_row_split_weighted_distributes_remainder);
    RUN_TEST(test_col_split_nested);
    return UNITY_END();
}
```

- [ ] **Step 2: Run to verify it fails to link**

Run: `pio test -e native -f test_midl_solve`
Expected: FAIL — `undefined reference to midl::solve_screen`.

- [ ] **Step 3: Implement leaf + split**

Create `src/midl_solve.cpp`:

```cpp
#include "midl_solve.h"

#include <string.h>

namespace midl {

namespace {

// Integer boundary for fraction `acc/total` of `span`. Using cumulative
// weights and flooring each boundary distributes the remainder deterministically
// and makes the last child end exactly at `span` (no gaps, no overlap).
inline int boundary(int span, long acc, long total) {
    return (int)((long)span * acc / total);
}

bool push_leaf(const char *id, Rect r, PlacementSet &out) {
    if (out.count >= FirmwareLimits::max_tiles_per_screen) return false;
    Placement &p = out.items[out.count++];
    memset(&p, 0, sizeof(p));
    strncpy(p.element, id, sizeof(p.element) - 1);
    p.rect = r;
    return true;
}

SolveStatus solve_node(JsonVariantConst node, Rect area, int depth, PlacementSet &out);

SolveStatus solve_split(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    JsonArrayConst kids = node["children"].as<JsonArrayConst>();
    if (kids.isNull() || kids.size() == 0) return SOLVE_BAD_NODE;
    bool row = strcmp(node["flow"] | node["dir"] | "row", "row") == 0;

    JsonArrayConst weights = node["weights"].as<JsonArrayConst>();
    long total = 0;
    for (size_t i = 0; i < kids.size(); i++)
        total += weights.isNull() ? 1 : (long)(weights[i] | 1);
    if (total <= 0) return SOLVE_BAD_NODE;

    long acc = 0;
    int span = row ? area.w : area.h;
    int prev = 0;
    for (size_t i = 0; i < kids.size(); i++) {
        acc += weights.isNull() ? 1 : (long)(weights[i] | 1);
        int next = boundary(span, acc, total);
        Rect cr = row ? Rect{area.x + prev, area.y, next - prev, area.h}
                      : Rect{area.x, area.y + prev, area.w, next - prev};
        prev = next;
        SolveStatus s = solve_node(kids[i], cr, depth + 1, out);
        if (s != SOLVE_OK) return s;
    }
    return SOLVE_OK;
}

SolveStatus solve_node(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    if (depth > FirmwareLimits::max_depth) return SOLVE_TOO_DEEP;
    if (node.containsKey("element"))
        return push_leaf(node["element"] | "", area, out) ? SOLVE_OK : SOLVE_TOO_MANY_TILES;
    if (node.containsKey("preset")) return expand_preset(node, area, out);
    if (node.containsKey("children")) return solve_split(node, area, depth, out);
    if (node.containsKey("cells")) return SOLVE_BAD_NODE;  // grid: Task 3
    return SOLVE_BAD_NODE;
}

}  // namespace

SolveStatus solve_screen(JsonVariantConst layout, Rect area, PlacementSet &out) {
    out.count = 0;
    return solve_node(layout, area, 1, out);
}

}  // namespace midl
```

(`expand_preset` is defined in Task 4; add a temporary forward stub returning `SOLVE_UNKNOWN_PRESET` if the linker needs it before Task 4 — the Task 2 tests use no presets.)

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_midl_solve`
Expected: 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/midl_solve.cpp test/test_midl_solve/ platformio.ini
git commit -m "feat(midl): integer split/leaf solver with remainder distribution"
```

---

### Task 3: Grid solving

**Files:**
- Modify: `src/midl_solve.cpp`
- Modify: `test/test_midl_solve/test_midl_solve.cpp`

- [ ] **Step 1: Add the failing grid test**

Append to `test/test_midl_solve/test_midl_solve.cpp` (before `main`, and add `RUN_TEST`s):

```cpp
void test_grid_2x2_rowmajor() {
    PlacementSet p = solve(
        R"({"rows":2,"cols":2,"cells":[{"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(4, p.count);
    TEST_ASSERT_EQUAL_INT(0,   find(p, "a")->rect.x); TEST_ASSERT_EQUAL_INT(0,   find(p, "a")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.x); TEST_ASSERT_EQUAL_INT(0,   find(p, "b")->rect.y);
    TEST_ASSERT_EQUAL_INT(0,   find(p, "c")->rect.x); TEST_ASSERT_EQUAL_INT(240, find(p, "c")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "d")->rect.x); TEST_ASSERT_EQUAL_INT(240, find(p, "d")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.w); TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.h);
}
```

Add `RUN_TEST(test_grid_2x2_rowmajor);` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_midl_solve`
Expected: FAIL — grid returns `SOLVE_BAD_NODE`, `p.count == 0`.

- [ ] **Step 3: Implement grid**

In `src/midl_solve.cpp`, replace the `cells` branch in `solve_node` with a call to `solve_grid`, and add `solve_grid` above `solve_node`:

```cpp
SolveStatus solve_grid(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    int rows = node["rows"] | 0, cols = node["cols"] | 0;
    JsonArrayConst cells = node["cells"].as<JsonArrayConst>();
    if (rows <= 0 || cols <= 0 || cells.isNull()) return SOLVE_BAD_NODE;
    if ((int)cells.size() != rows * cols) return SOLVE_BAD_NODE;  // row-major, full
    for (int i = 0; i < (int)cells.size(); i++) {
        int r = i / cols, c = i % cols;
        int x0 = area.x + boundary(area.w, c, cols), x1 = area.x + boundary(area.w, c + 1, cols);
        int y0 = area.y + boundary(area.h, r, rows), y1 = area.y + boundary(area.h, r + 1, rows);
        SolveStatus s = solve_node(cells[i], Rect{x0, y0, x1 - x0, y1 - y0}, depth + 1, out);
        if (s != SOLVE_OK) return s;
    }
    return SOLVE_OK;
}
```

Change the `solve_node` line `if (node.containsKey("cells")) return SOLVE_BAD_NODE;` to:

```cpp
    if (node.containsKey("cells")) return solve_grid(node, area, depth, out);
```

(Forward-declare `solve_grid` alongside `solve_node` if needed.)

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_midl_solve`
Expected: 5 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/midl_solve.cpp test/test_midl_solve/
git commit -m "feat(midl): row-major integer grid solving"
```

---

### Task 4: Preset expansion (`full`, `hero-split`)

**Files:**
- Modify: `src/midl_solve.cpp`
- Modify: `test/test_midl_solve/test_midl_solve.cpp`

- [ ] **Step 1: Add the failing preset test**

Append to `test/test_midl_solve/test_midl_solve.cpp` (and add `RUN_TEST`s):

```cpp
void test_preset_full() {
    PlacementSet p = solve(R"({"preset":"full","slots":["a"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    TEST_ASSERT_EQUAL_INT(480, find(p, "a")->rect.w);
}

void test_preset_hero_split() {
    // {1,{2,3}} = row[ leaf, col[leaf,leaf] ], equal weights.
    PlacementSet p = solve(R"({"preset":"hero-split","slots":["hero","x","y"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    TEST_ASSERT_EQUAL_INT(0,   find(p, "hero")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "hero")->rect.w);
    TEST_ASSERT_EQUAL_INT(240, find(p, "x")->rect.x);
    TEST_ASSERT_EQUAL_INT(0,   find(p, "x")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "y")->rect.y);
}

void test_preset_unknown_rejected() {
    JsonDocument doc; deserializeJson(doc, R"({"preset":"nope","slots":["a"]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_UNKNOWN_PRESET,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0,0,480,480}, out));
}
```

Add the three `RUN_TEST`s to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_midl_solve`
Expected: FAIL (unknown preset / stub).

- [ ] **Step 3: Implement `expand_preset`**

In `src/midl_solve.cpp`, implement `expand_preset` (replace any Task 2 stub). It solves the preset's geometry directly using the slot ids as leaves:

```cpp
SolveStatus expand_preset(JsonVariantConst node, Rect area, PlacementSet &out) {
    const char *name = node["preset"] | "";
    JsonArrayConst slots = node["slots"].as<JsonArrayConst>();
    if (slots.isNull()) return SOLVE_BAD_NODE;

    if (strcmp(name, "full") == 0) {
        if (slots.size() < 1) return SOLVE_BAD_NODE;
        return push_leaf(slots[0] | "", area, out) ? SOLVE_OK : SOLVE_TOO_MANY_TILES;
    }
    if (strcmp(name, "hero-split") == 0) {
        if (slots.size() != 3) return SOLVE_BAD_NODE;
        // row[ hero(1) | col[ s1, s2 ] ] with equal weights -> hero gets left half.
        int mid = boundary(area.w, 1, 2);
        Rect left{area.x, area.y, mid, area.h};
        Rect right{area.x + mid, area.y, area.w - mid, area.h};
        int rmid = boundary(right.h, 1, 2);
        if (!push_leaf(slots[0] | "", left, out)) return SOLVE_TOO_MANY_TILES;
        if (!push_leaf(slots[1] | "", Rect{right.x, right.y, right.w, rmid}, out)) return SOLVE_TOO_MANY_TILES;
        if (!push_leaf(slots[2] | "", Rect{right.x, right.y + rmid, right.w, right.h - rmid}, out)) return SOLVE_TOO_MANY_TILES;
        return SOLVE_OK;
    }
    return SOLVE_UNKNOWN_PRESET;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_midl_solve`
Expected: 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/midl_solve.cpp test/test_midl_solve/
git commit -m "feat(midl): preset expansion (full, hero-split) via direct solve"
```

---

### Task 5: Guard rails — depth, capacity, malformed

**Files:**
- Modify: `test/test_midl_solve/test_midl_solve.cpp`

- [ ] **Step 1: Add failing guard tests**

Append (and add `RUN_TEST`s):

```cpp
void test_too_deep_rejected() {
    // depth 4 > MAX_LAYOUT_DEPTH (3): row>col>row>leaf
    JsonDocument doc;
    deserializeJson(doc, R"({"flow":"row","children":[{"flow":"col","children":[
        {"flow":"row","children":[{"element":"a"}]}]}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_DEEP,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0,0,480,480}, out));
}

void test_too_many_tiles_rejected() {
    // 5 leaves in a row > max_tiles_per_screen (4)
    JsonDocument doc;
    deserializeJson(doc, R"({"flow":"row","children":[
        {"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"},{"element":"e"}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_MANY_TILES,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0,0,480,480}, out));
}

void test_malformed_node_rejected() {
    JsonDocument doc; deserializeJson(doc, R"({"frobnicate":true})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_BAD_NODE,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0,0,480,480}, out));
}
```

- [ ] **Step 2: Run — verify they already pass**

Run: `pio test -e native -f test_midl_solve`
Expected: 11 tests PASS. (The guards were implemented in Tasks 2–4: depth check at `solve_node` entry, capacity in `push_leaf`, default `SOLVE_BAD_NODE`.) If `test_too_deep_rejected` fails because depth is checked too late, move the `depth > max_depth` check to the very first line of `solve_node` — it must reject **before** recursing, never after.

- [ ] **Step 3: Commit**

```bash
git add test/test_midl_solve/
git commit -m "test(midl): solver rejects over-deep, over-capacity, malformed trees"
```

---

### Task 6: Parity — solve the Plan 5 projection fixtures

**Files:**
- Modify: `test/test_midl_solve/test_midl_solve.cpp`

- [ ] **Step 1: Add the failing parity test**

This proves the solver produces the expected geometry for the real fixtures Plan 5 shipped (and that Plan 6's renderer + the web renderer will agree). Append:

```cpp
#include <stdio.h>
#include <string>

static std::string slurp2(const char *path) {
    FILE *f = fopen(path, "rb"); TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::string s; char b[1024]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f); return s;
}

void test_steering_fixture_geometry() {
    std::string j = slurp2("test/fixtures/yb-midl/projection/steering-4tile.square-480.json");
    JsonDocument doc; deserializeJson(doc, j);
    JsonVariantConst layout = doc["screens"][0]["layout"];
    PlacementSet p; 
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_OK, midl::solve_screen(layout, {0,0,480,480}, p));
    TEST_ASSERT_EQUAL_size_t(4, p.count);
    // row weights [3,2] over 480: compass = [0,288), right col = [288,480).
    TEST_ASSERT_EQUAL_INT(0,   find(p, "compass")->rect.x);
    TEST_ASSERT_EQUAL_INT(288, find(p, "compass")->rect.w);
    TEST_ASSERT_EQUAL_INT(288, find(p, "cts")->rect.x);
    TEST_ASSERT_EQUAL_INT(192, find(p, "cts")->rect.w);
    // right column split into 3 equal rows over 480: 160 each.
    TEST_ASSERT_EQUAL_INT(0,   find(p, "cts")->rect.y);
    TEST_ASSERT_EQUAL_INT(160, find(p, "xte")->rect.y);
    TEST_ASSERT_EQUAL_INT(320, find(p, "rudder")->rect.y);
}
```

Add `RUN_TEST(test_steering_fixture_geometry);` to `main`.

- [ ] **Step 2: Run to verify pass**

Run: `pio test -e native -f test_midl_solve`
Expected: 12 tests PASS. If the fixture still uses `dir` (not migrated in Task 1), the `flow|dir` fallback still solves it — but ensure Task 1's migration ran so the canonical key is consistent.

- [ ] **Step 3: Commit + push**

```bash
git add test/test_midl_solve/
git commit -m "test(midl): solver parity against steering projection fixture"
git push origin <branch>
```

---

### Slice 1 Self-Review

- **Spec §3.3 solving contract:** integer math (Task 2 `boundary`), row-major grid with `cells == rows*cols` (Task 3), no-overlap/in-bounds by construction (cumulative boundaries), depth limit enforced as it descends (Task 5). Safe-area/gutter/padding + min-rect rejection are **deferred to Slice 2** (they need the manifest's per-class metrics) — noted, not dropped.
- **Reuses, not rebuilds:** binding resolution via the landed `PathStore` (Slice 2); painters already exist (Slice 2). Slice 1 adds geometry only.
- **No placeholders / type consistency:** `SolveStatus`, `PlacementSet`, `solve_screen`, `expand_preset`, `boundary`, `push_leaf` names are consistent across Tasks 1–6. ✓

---

## Slice map (Slices 2–5 detailed just-in-time after Slice 1 lands & is reviewed)

| Slice | What | New work | Couples with | Flagged decision |
|---|---|---|---|---|
| **1. Solver** (this) | Integer split/grid/preset solver, host-tested vs fixtures | `midl_solve.*`, tests | Plan 5 | `dir`→`flow` (resolved: accept both) |
| 2. Render core | Parent-agnostic painters: build an LVGL tree from a `PlacementSet`, bindings via `PathStore`; safe-area/gutter/padding + min-rect from manifest; one screen behind a flag; on the UI task via a new `ConfigApplyMidl` app-event | `midl_render.*`, refactor `ui/ui_layouts.cpp` painters to `(parent, Rect, element)` form, `app_events.cpp`, `layout_renderer.cpp` | Slice 1; [[feedback-lvgl-only-on-ui-task]] | min-rect source = manifest `minRect` per element |
| 3. Persistence & rollback | LittleFS partition holding `current`/`last-known-good`/`factory`; A/B apply: write `pending` → render → promote after a stability window; boot-loop counter + pending flag in NVS (`storage::Namespace`) → auto-revert to last-good then factory | partition table, `midl_store.*`, `main.cpp` boot path | Slice 2 | **Storage = LittleFS** (overrides spec's `s_last_json`-only) |
| 4. Delivery, actions & validation | `POST /api/midl/config` + `GET` + `POST /api/midl/reset?to=default\|previous`; SignalK-pull fallback; on-device render-safety validation (structural + capability vs embedded manifest + geometry); action **execution** (nav/put/command via `net::dispatchCommand`) | `web.cpp`, `midl_validate.*`, `net`/dispatch | Slices 2–3; manager-migration Slice 5 | **No action gating; validation = reliability not security** (overrides spec §3.4/§3.6) |
| 5. Cutover | Replace the `TemplateId` path for authored screens; retire bespoke `screen_*.cpp` / legacy `layout_renderer` mapping where MIDL now covers them; keep `layout::parse` memset/bounds guards as the floor; full sim + on-device soak | `ui/ui_layouts.cpp`, `main.cpp`, remove dead paths | Slices 1–4 | none |

**Slice 1 deliverable = a host-tested geometry engine** with zero device-behaviour change. Each later slice ships independently and is reviewed before the next is detailed.

---

## Testing (whole plan)

- **Host (`pio test -e native`):** solver geometry (Slice 1), validation accept/reject corpus (Slice 4), store tier round-trips (Slice 3). Reuse the shared golden corpus so the native and JS solvers agree (spec §3.6 cross-implementation parity).
- **Sim (`make sim`):** each projection fixture renders with no tile overlap, in-bounds, at 480×480 (Slice 2+).
- **Device (`make ota-verify`, `espdisp soak`):** apply a projection over `POST /api/midl/config`, confirm render + a bad doc rejects without wedging the screen + a crash-doc auto-rolls-back (Slices 3–4). Soak gates the stability claim ([[project-soak-and-lab-access]]).

## Constraints to preserve (CLAUDE.md memory traps)

`memset` not `out = Config{}`; live config + apply blob in PSRAM; no big scratch on task-callback stacks; depth bounded as the solver descends; LittleFS/NVS writes off the UI task; all `lv_obj_*` on the UI task.
