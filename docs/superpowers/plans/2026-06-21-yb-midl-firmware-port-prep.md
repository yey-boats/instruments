# YB-MIDL Firmware-Port Preparation (Plan 5) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock the firmware-facing contract for native MIDL rendering *before* writing any renderer: define the **resolved single-class projection** the device will accept, pin the per-class limits to the firmware POD, and add host-test guards that fail if a projection or a future POD change would exceed what the device can hold.

**Architecture:** No device behaviour changes. This plan adds (a) a small set of **golden projection fixtures** — resolved, single-class, preset-expanded MIDL documents that Plan 6's parser will consume — and (b) **host tests** that assert every fixture fits the firmware POD bounds (`MAX_SCREENS`, `MAX_TILES_PER_SCREEN`, `maxDepth`, string maxima) and that the embedded manifest never advertises more than the firmware can render. It is pure host/CI work (`pio test -e native`).

**Tech Stack:** C++17, PlatformIO `native` env + Unity, ArduinoJson v7, the MIDL submodule fixtures (`midl/web` golden corpus).

This is **Plan 5 of the YB-MIDL runtime spec** (`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md` §5 item 5). It unblocks **Plan 6** (the render port) and resolves the spec's one *Still open* question (§10): firmware class limits.

---

## Context an out-of-context engineer needs

- The firmware layout POD is `layout::Config` in `include/layout.h`: `Config → Screen[MAX_SCREENS=8] → Tile[MAX_TILES_PER_SCREEN=4]`, with `STR_LEN=32`, `PATH_LEN=96`, `MAX_PATHS_PER_OBJECT=6`, `MAX_ALARMS=8`. It is **PSRAM-allocated** at runtime (`layout_loader.cpp`) and must never be stack-built (34 KB temp boot-loops the device — see CLAUDE.md "Memory traps").
- The embedded capability manifest is `include/generated_midl_manifest.h` (`midl_manifest::JSON`), generated from `midl/cpp/include/yb_midl_catalog.h` via `tools/embed_manifest.py square-480`. For the `square-480` class it declares `maxTiles: 4`, `maxDepth: 3`, presets `["full","hero-split"]`, and the 9 element types.
- The **resolved single-class projection** is the JSON the *device* will parse in Plan 6: a MIDL config document that the manager has already (1) migrated to the device's MIDL version, (2) resolved to the device's single resolution class (variants collapsed), and (3) optionally left presets in (the device expands the small finite set itself). It therefore has **no `variants[]`** and targets exactly one class.
- Native test convention (from `docs/superpowers/plans/2026-06-17-slice1-generic-path-store.md`): each test lives in `test/test_<name>/`, and the source-under-test plus the test are added to `platformio.ini`'s native `build_src_filter` / `test_filter`. The native env builds only the pure-C++ TUs.

---

## File Structure

- `test/fixtures/yb-midl/projection/` (create) — golden resolved single-class projection docs the device will accept. One JSON file per shape. Shared by this plan and Plan 6.
  - `full-1tile.square-480.json` — a single full-screen value.
  - `hero-split-3tile.square-480.json` — `hero-split` preset, 3 leaves.
  - `steering-4tile.square-480.json` — nested split, 4 leaves, the worst-case shape (depth 3, tiles 4).
- `include/midl_limits.h` (create) — one header that re-exposes the firmware POD bounds as named `constexpr` + a single struct `midl::FirmwareLimits` so tests and Plan 6 read them from one place (DRY).
- `test/test_midl_projection/test_midl_projection.cpp` (create) — host tests: each fixture parses and fits `FirmwareLimits`; the embedded manifest's `square-480` limits do not exceed `FirmwareLimits`.
- `platformio.ini` (modify) — add `test_midl_projection` to the native `test_filter`.

---

### Task 1: Pin firmware limits in one header

**Files:**
- Create: `include/midl_limits.h`

- [ ] **Step 1: Create the header**

Create `include/midl_limits.h`:

```cpp
#pragma once

// Single source of truth for the firmware-facing MIDL projection bounds.
// These mirror the layout POD limits (include/layout.h) and the values the
// square-480 capability manifest advertises. Plan 6's parser/solver and the
// host guard tests both read them from here so a POD change and a manifest
// change cannot silently diverge.

#include <stddef.h>

#include "layout.h"  // layout::MAX_SCREENS, MAX_TILES_PER_SCREEN, STR_LEN, PATH_LEN

namespace midl {

// Maximum nesting depth of a layout node tree the firmware solver will walk.
// Matches the square-480 manifest `maxDepth`. Enforced AS the solver descends
// (a post-hoc depth check would already have overflowed the 8 KB task stack —
// CLAUDE.md silent-reboot trap), so it is a hard recursion bound, not advice.
constexpr int MAX_LAYOUT_DEPTH = 3;

// A resolved single-class projection the device accepts must fit all of these.
struct FirmwareLimits {
    static constexpr size_t max_screens = layout::MAX_SCREENS;            // 8
    static constexpr size_t max_tiles_per_screen = layout::MAX_TILES_PER_SCREEN;  // 4
    static constexpr int max_depth = MAX_LAYOUT_DEPTH;                    // 3
    static constexpr size_t str_len = layout::STR_LEN;                    // 32 (incl NUL)
    static constexpr size_t path_len = layout::PATH_LEN;                  // 96 (incl NUL)
};

}  // namespace midl
```

- [ ] **Step 2: Commit**

```bash
git add include/midl_limits.h
git commit -m "feat(midl): pin firmware projection limits in one header"
```

---

### Task 2: Golden projection fixtures

**Files:**
- Create: `test/fixtures/yb-midl/projection/full-1tile.square-480.json`
- Create: `test/fixtures/yb-midl/projection/hero-split-3tile.square-480.json`
- Create: `test/fixtures/yb-midl/projection/steering-4tile.square-480.json`

- [ ] **Step 1: Single full-screen value**

Create `test/fixtures/yb-midl/projection/full-1tile.square-480.json`:

```json
{
  "midl": "1.0.0",
  "class": "square-480",
  "settings": { "defaultScreen": "sog" },
  "screens": [
    {
      "id": "sog",
      "elements": {
        "sog": {
          "type": "single-value",
          "name": "SOG",
          "format": { "unit": "kn", "precision": 1 },
          "bindings": { "value": { "kind": "signalk", "path": "navigation.speedOverGround" } }
        }
      },
      "layout": { "element": "sog" }
    }
  ]
}
```

- [ ] **Step 2: hero-split, 3 leaves (preset left unexpanded — device expands)**

Create `test/fixtures/yb-midl/projection/hero-split-3tile.square-480.json`:

```json
{
  "midl": "1.0.0",
  "class": "square-480",
  "screens": [
    {
      "id": "dash",
      "elements": {
        "wind": { "type": "windrose", "bindings": {
          "value": { "kind": "signalk", "path": "environment.wind.speedApparent" },
          "dir":   { "kind": "signalk", "path": "environment.wind.angleApparent" } } },
        "sog": { "type": "single-value", "name": "SOG", "format": { "unit": "kn" },
          "bindings": { "value": { "kind": "signalk", "path": "navigation.speedOverGround" } } },
        "depth": { "type": "single-value", "name": "DEPTH", "format": { "unit": "m" },
          "bindings": { "value": { "kind": "signalk", "path": "environment.depth.belowTransducer" } } }
      },
      "layout": { "preset": "hero-split", "slots": ["wind", "sog", "depth"] }
    }
  ]
}
```

- [ ] **Step 3: steering, 4 leaves, worst-case depth-3 nested split**

Create `test/fixtures/yb-midl/projection/steering-4tile.square-480.json`:

```json
{
  "midl": "1.0.0",
  "class": "square-480",
  "screens": [
    {
      "id": "steering",
      "elements": {
        "compass": { "type": "compass", "bindings": {
          "value": { "kind": "signalk", "path": "navigation.headingTrue" },
          "dir":   { "kind": "signalk", "path": "navigation.courseRhumbline.bearingTrackTrue" } },
          "markers": [ { "glyph": "triangle" }, { "glyph": "diamond" } ] },
        "cts": { "type": "single-value", "name": "CTS", "format": { "unit": "deg" },
          "bindings": { "value": { "kind": "signalk", "path": "navigation.courseRhumbline.bearingTrackTrue" } } },
        "xte": { "type": "single-value", "name": "XTE", "format": { "unit": "m" },
          "bindings": { "value": { "kind": "signalk", "path": "navigation.courseRhumbline.crossTrackError" } } },
        "rudder": { "type": "gauge", "name": "RUDDER", "format": { "range": [-35, 35] },
          "bindings": { "value": { "kind": "signalk", "path": "steering.rudderAngle" } } }
      },
      "layout": {
        "dir": "row",
        "weights": [3, 2],
        "children": [
          { "element": "compass" },
          { "dir": "col", "children": [ { "element": "cts" }, { "element": "xte" }, { "element": "rudder" } ] }
        ]
      }
    }
  ]
}
```

> Note: the node split key is `dir` to match the **firmware/spec EBNF** in
> `2026-06-19-generic-dashboard-runtime-design.md` §3.6 (`split{ dir: row|col }`).
> The MIDL submodule schema renamed this to `flow` (commit `83ed4e0`). **This
> mismatch is a real decision Plan 6 Task 1 must resolve** (accept `flow`,
> accept both, or realign the spec). The fixtures here use `dir`; if Plan 6
> standardises on `flow`, update these three files in the same commit.

- [ ] **Step 4: Commit**

```bash
git add test/fixtures/yb-midl/projection/
git commit -m "test(midl): golden firmware projection fixtures (1/3/4-tile square-480)"
```

---

### Task 3: Host guard — fixtures fit firmware POD bounds

**Files:**
- Create: `test/test_midl_projection/test_midl_projection.cpp`
- Modify: `platformio.ini` (native `test_filter`)

- [ ] **Step 1: Write the failing test**

Create `test/test_midl_projection/test_midl_projection.cpp`:

```cpp
// Host guard: every shipped firmware-projection fixture must fit the
// firmware POD bounds, and the embedded manifest must never advertise a
// square-480 limit larger than the firmware can hold. Pure host test —
// no device, no LVGL.

#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "midl_limits.h"

// Counts leaves and measures max depth of a node tree. `dir`/`flow` split,
// `grid` cells, `preset` (counts slots as leaves), or a leaf `{element}`.
static void walk(JsonVariantConst node, int depth, int &leaves, int &maxDepth) {
    if (depth > maxDepth) maxDepth = depth;
    if (node.containsKey("element")) { leaves++; return; }
    if (node.containsKey("preset")) { leaves += node["slots"].as<JsonArrayConst>().size(); return; }
    JsonArrayConst kids = node.containsKey("children") ? node["children"].as<JsonArrayConst>()
                                                       : node["cells"].as<JsonArrayConst>();
    for (JsonVariantConst k : kids) walk(k, depth + 1, leaves, maxDepth);
}

static std::string slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::string s; char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void check_fixture(const char *path) {
    std::string json = slurp(path);
    JsonDocument doc;
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, json), path);

    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    TEST_ASSERT_LESS_OR_EQUAL_size_t(midl::FirmwareLimits::max_screens, screens.size());

    for (JsonVariantConst scr : screens) {
        int leaves = 0, maxDepth = 0;
        walk(scr["layout"], 1, leaves, maxDepth);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(midl::FirmwareLimits::max_tiles_per_screen, leaves, path);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(midl::FirmwareLimits::max_depth, maxDepth, path);
    }
}

void test_full_1tile_fits()   { check_fixture("test/fixtures/yb-midl/projection/full-1tile.square-480.json"); }
void test_hero_split_fits()   { check_fixture("test/fixtures/yb-midl/projection/hero-split-3tile.square-480.json"); }
void test_steering_4tile_fits(){ check_fixture("test/fixtures/yb-midl/projection/steering-4tile.square-480.json"); }

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_full_1tile_fits);
    RUN_TEST(test_hero_split_fits);
    RUN_TEST(test_steering_4tile_fits);
    return UNITY_END();
}
```

- [ ] **Step 2: Add the test to the native filter**

In `platformio.ini`, under `[env:native]`, add `test_midl_projection` to `test_filter` (keep existing entries):

```ini
test_filter = test_parser, test_layout, test_path_store, test_midl_projection
```

(If `test_filter` lists different existing entries, append `test_midl_projection` to whatever is there — do not drop the others.)

- [ ] **Step 3: Run the test**

Run: `pio test -e native -f test_midl_projection`
Expected: 3 tests PASS. If a fixture fails, the fixture is over budget — fix the fixture, not the limit.

- [ ] **Step 4: Commit**

```bash
git add test/test_midl_projection/ platformio.ini
git commit -m "test(midl): guard projection fixtures fit firmware POD bounds"
```

---

### Task 4: Lock class limits — manifest must not exceed firmware

**Files:**
- Modify: `test/test_midl_projection/test_midl_projection.cpp`

- [ ] **Step 1: Add the failing manifest-vs-firmware test**

Append to `test/test_midl_projection/test_midl_projection.cpp` (before `main`):

```cpp
#include "generated_midl_manifest.h"  // midl_manifest::JSON, embedded square-480

void test_manifest_within_firmware_limits() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, midl_manifest::JSON));
    bool sawSquare = false;
    for (JsonVariantConst cls : doc["classes"].as<JsonArrayConst>()) {
        if (std::string(cls["id"] | "") != "square-480") continue;
        sawSquare = true;
        // The device must be able to HOLD whatever the manifest advertises.
        TEST_ASSERT_LESS_OR_EQUAL_INT(midl::FirmwareLimits::max_tiles_per_screen, cls["maxTiles"] | 999);
        TEST_ASSERT_LESS_OR_EQUAL_INT(midl::FirmwareLimits::max_depth, cls["maxDepth"] | 999);
    }
    TEST_ASSERT_TRUE_MESSAGE(sawSquare, "square-480 class missing from embedded manifest");
}
```

Add `RUN_TEST(test_manifest_within_firmware_limits);` to `main`.

> **Decision locked here (spec §10 *Still open*):** the firmware-accepted
> projection is capped at `MAX_TILES_PER_SCREEN = 4` / `maxDepth = 3` for
> `square-480`. Larger landscape classes (800×480, 1024×600) remain
> **preview-only** in the web renderer until a measured POD expansion lands;
> this test fails the build if the manifest ever advertises a square-480 limit
> the firmware can't hold.

- [ ] **Step 2: Run + verify pass**

Run: `pio test -e native -f test_midl_projection`
Expected: 4 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add test/test_midl_projection/
git commit -m "test(midl): lock square-480 manifest limits to firmware POD"
```

---

### Task 5: Document the POD-growth budget for Plan 6

**Files:**
- Modify: `test/test_midl_projection/test_midl_projection.cpp`

- [ ] **Step 1: Add a size-budget guard for the layout POD**

Append to `test/test_midl_projection/test_midl_projection.cpp` (before `main`):

```cpp
#include "layout.h"

// The live layout::Config is PSRAM-allocated, but its size still bounds the
// PSRAM apply blob and the projection the device holds. Plan 6 will add a
// node-tree representation; this guard records today's size so that growth is
// a conscious, reviewed change rather than an accident. Update the ceiling in
// the SAME commit that grows the POD, with a note why.
void test_config_pod_size_budget() {
    // Today ~34 KB. Ceiling set with headroom for Plan 6's node tree.
    TEST_ASSERT_LESS_OR_EQUAL_UINT(48u * 1024u, (unsigned)sizeof(layout::Config));
    printf("[midl] sizeof(layout::Config) = %u bytes\n", (unsigned)sizeof(layout::Config));
}
```

Add `RUN_TEST(test_config_pod_size_budget);` to `main`.

- [ ] **Step 2: Run + read the printed size**

Run: `pio test -e native -f test_midl_projection -v`
Expected: 5 tests PASS; note the printed `sizeof(layout::Config)` — record it in the Plan 6 architecture section as the baseline.

- [ ] **Step 3: Commit**

```bash
git add test/test_midl_projection/
git commit -m "test(midl): record layout::Config POD-size budget for the render port"
```

- [ ] **Step 4: Push**

```bash
git push origin <branch>
```

---

## Self-Review

- **Spec §5 item 5 coverage:** "measure POD growth" → Task 5; "decide class limits" → Task 4 (locks cap=4, resolves §10 open question); "define the resolved single-class projection" → Tasks 1–3 (limits header + fixtures + fit guard). ✓
- **No device behaviour change:** all tasks are `test/` + one header; no `src/` render path touched. ✓
- **`dir` vs `flow`:** flagged explicitly in Task 2 Step 3 as a decision Plan 6 owns; fixtures use `dir` per the firmware spec EBNF, with a one-commit migration note. ✓
- **Type consistency:** `midl::FirmwareLimits` and `midl::MAX_LAYOUT_DEPTH` defined in Task 1 are the only limit names used in Tasks 3–5. ✓
