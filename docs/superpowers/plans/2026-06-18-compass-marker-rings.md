# Compass Marker Rings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render a configurable list of up to 12 markers (HDG/COG/CTS by default) on the firmware's compass-like widgets, via one shared marker primitive, with host-tested placement math and sim-verified visuals.

**Architecture:** Split pure marker math (placement angle, occlusion, glyph↔token map) into a host-compiled `marker_math` module unit-tested under `[env:native]`; put the LVGL glyph drawing + orbiting `MarkerRing` in a device/sim `ui_markers` module that consumes the pure math. Built-in screens (steering tile, autopilot HUD, wind rose, wind-steer) build a default marker list and feed each marker's bearing from the existing `metric_scalar()` resolver each refresh. The capability manifest gains the glyph set + `maxMarkersPerDial`.

**Tech Stack:** C++17, LVGL 9, PlatformIO (`native` Unity tests, `sim*` headless render envs), `heap_caps_*` PSRAM allocation.

**Spec:** `docs/superpowers/specs/2026-06-18-compass-marker-rings-design.md`

**Scope note:** This plan covers the firmware (this repo). The manager preview renderers (`signalk-espdisp-manager` repo) and the full authored-marker config-schema parsing are the umbrella spec's editor slice (`2026-06-17-device-mirrored-layout-editor-design.md` §5) and are **out of scope here** — this pass renders built-in marker lists and reports the manifest the editor will later gate against.

---

## File Structure

- **Create** `include/marker_math.h` — pure placement/occlusion/glyph-token API (no LVGL).
- **Create** `src/marker_math.cpp` — pure implementation (added to `[env:native]` build).
- **Create** `test/test_marker_math/test_marker_math.cpp` — Unity host tests.
- **Create** `include/ui_markers.h` — `Glyph` enum, `MarkerSpec`, `MarkerRing`, `draw_glyph`, build/update (LVGL).
- **Create** `src/ui/ui_markers.cpp` — LVGL glyph drawing + orbiting marker ring.
- **Modify** `src/ui/ui_layouts.cpp` — steering tile Compass + wind rose adopt `MarkerRing`.
- **Modify** `src/ui/ui_compass.cpp`, `include/ui_compass.h` — semicircular HUD exposes a `MarkerRing`.
- **Modify** `src/ui/screen_autopilot.cpp` — drive HDG/COG/CTS + target markers.
- **Modify** `src/ui/screen_wind_steer.cpp` — drive wind/heading markers (read first; wire like AP).
- **Modify** `src/capabilities.cpp` + `test/test_capabilities/test_capabilities.cpp` — report `glyphs` + `maxMarkersPerDial`.
- **Modify** `platformio.ini` — add `src/marker_math.cpp` to `[env:native]` `build_src_filter` and `test_marker_math` to `test_filter`; add `ui_markers.cpp` to each `sim*` filter that renders a compass.
- **Modify** `tools/sim_render.sh` / docs — regenerate previews (Task 9).

---

## Task 1: Pure marker math — header + first failing test

**Files:**
- Create: `include/marker_math.h`
- Create: `test/test_marker_math/test_marker_math.cpp`
- Modify: `platformio.ini:394` (native `build_src_filter`), `platformio.ini:405-428` (`test_filter`)

- [ ] **Step 1: Write the header**

`include/marker_math.h`:

```cpp
#pragma once

// Pure (no-LVGL) marker-ring math: where a marker sits on a dial and whether a
// semicircular dial occludes it, plus the glyph token<->enum map the editor and
// firmware share. Host-compiled and unit-tested under [env:native]; the LVGL
// rendering lives in ui_markers.{h,cpp} and calls into this.

#include <math.h>
#include <stdint.h>

namespace ui {

// Keep in lockstep with ui::Glyph in ui_markers.h and the manifest "glyphs"
// list in src/capabilities.cpp. marker_math owns the canonical order so the
// token<->index map is testable without LVGL.
enum class GlyphId : uint8_t {
    Triangle = 0,
    Diamond,
    Circle,
    Bar,
    Cross,
    ChevronIn,
    ChevronOut,
    ChevronLeft,
    ChevronRight,
    ChevronDouble,
    COUNT
};

constexpr uint8_t kMaxMarkersPerDial = 12;

// Screen angle of a marker: value - reference, normalized to [0,360).
// 0 = top of the dial (under the lubber). NaN in -> NaN out (caller hides it).
inline double marker_screen_angle(double value_deg, double reference_deg) {
    if (isnan(value_deg) || isnan(reference_deg)) return NAN;
    double a = value_deg - reference_deg;
    a = fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

// True if a semicircular (top-half) dial occludes this screen angle. The top
// half spans [-half_window, +half_window] around 0; anything further round is
// in the hidden lower half. Matches the degree-label hide at |rel| > 96 in
// ui_compass.cpp. NaN -> occluded (hidden).
inline bool marker_occluded(double screen_angle_deg, double half_window_deg) {
    if (isnan(screen_angle_deg)) return true;
    double rel = screen_angle_deg;
    if (rel > 180.0) rel -= 360.0;  // -> [-180,180]
    return rel > half_window_deg || rel < -half_window_deg;
}

// Glyph token (manifest/editor string) -> GlyphId. Returns COUNT on unknown.
GlyphId glyph_from_token(const char *token);

// GlyphId -> stable token string. Returns "" for COUNT/out-of-range.
const char *glyph_to_token(GlyphId g);

}  // namespace ui
```

- [ ] **Step 2: Write the failing test**

`test/test_marker_math/test_marker_math.cpp`:

```cpp
#include <math.h>
#include <string.h>
#include <unity.h>

#include "marker_math.h"

using ui::GlyphId;

void setUp() {}
void tearDown() {}

static void test_screen_angle_same_value_is_zero() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, ui::marker_screen_angle(127.0, 127.0));
}

static void test_screen_angle_wraps_positive() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 350.0, ui::marker_screen_angle(10.0, 20.0));
}

static void test_screen_angle_relative() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 30.0, ui::marker_screen_angle(120.0, 90.0));
}

static void test_screen_angle_nan_propagates() {
    TEST_ASSERT_TRUE(isnan(ui::marker_screen_angle(NAN, 10.0)));
    TEST_ASSERT_TRUE(isnan(ui::marker_screen_angle(10.0, NAN)));
}

static void test_occluded_top_visible() {
    TEST_ASSERT_FALSE(ui::marker_occluded(0.0, 96.0));
    TEST_ASSERT_FALSE(ui::marker_occluded(90.0, 96.0));
    TEST_ASSERT_FALSE(ui::marker_occluded(300.0, 96.0));  // -60 deg
}

static void test_occluded_bottom_hidden() {
    TEST_ASSERT_TRUE(ui::marker_occluded(120.0, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(180.0, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(NAN, 96.0));
}

static void test_glyph_token_roundtrip() {
    for (uint8_t i = 0; i < (uint8_t)GlyphId::COUNT; ++i) {
        const char *tok = ui::glyph_to_token((GlyphId)i);
        TEST_ASSERT_TRUE(tok[0] != 0);
        TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)ui::glyph_from_token(tok));
    }
}

static void test_glyph_token_unknown() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlyphId::COUNT, (uint8_t)ui::glyph_from_token("nope"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_screen_angle_same_value_is_zero);
    RUN_TEST(test_screen_angle_wraps_positive);
    RUN_TEST(test_screen_angle_relative);
    RUN_TEST(test_screen_angle_nan_propagates);
    RUN_TEST(test_occluded_top_visible);
    RUN_TEST(test_occluded_bottom_hidden);
    RUN_TEST(test_glyph_token_roundtrip);
    RUN_TEST(test_glyph_token_unknown);
    return UNITY_END();
}
```

- [ ] **Step 3: Register the test + source in `platformio.ini`**

At the end of `[env:native]` `build_src_filter` (line 394), append: ` +<marker_math.cpp>`
In `[env:native]` `test_filter` (after `test_subscription_set`, line 428), add a line: `    test_marker_math`

- [ ] **Step 4: Run the test, verify it fails to link**

Run: `pio test -e native -f test_marker_math`
Expected: FAIL — undefined reference to `ui::glyph_from_token` / `ui::glyph_to_token` (inlines pass; the two map functions are not yet defined).

- [ ] **Step 5: Commit**

```bash
git add include/marker_math.h test/test_marker_math/test_marker_math.cpp platformio.ini
git commit -m "test(markers): pure marker-math placement + glyph-token tests (red)"
```

---

## Task 2: Pure marker math — implementation

**Files:**
- Create: `src/marker_math.cpp`

- [ ] **Step 1: Write the implementation**

`src/marker_math.cpp`:

```cpp
#include "marker_math.h"

#include <string.h>

namespace ui {

// Canonical token table, indexed by GlyphId. Order MUST match the enum and the
// manifest "glyphs" list in src/capabilities.cpp.
static const char *const kGlyphTokens[(uint8_t)GlyphId::COUNT] = {
    "triangle", "diamond",       "circle",        "bar",          "cross",
    "chevron_in", "chevron_out", "chevron_left", "chevron_right", "chevron_double",
};

GlyphId glyph_from_token(const char *token) {
    if (!token) return GlyphId::COUNT;
    for (uint8_t i = 0; i < (uint8_t)GlyphId::COUNT; ++i) {
        if (strcmp(token, kGlyphTokens[i]) == 0) return (GlyphId)i;
    }
    return GlyphId::COUNT;
}

const char *glyph_to_token(GlyphId g) {
    if ((uint8_t)g >= (uint8_t)GlyphId::COUNT) return "";
    return kGlyphTokens[(uint8_t)g];
}

}  // namespace ui
```

- [ ] **Step 2: Run the test, verify it passes**

Run: `pio test -e native -f test_marker_math`
Expected: PASS (8 tests).

- [ ] **Step 3: Run the full native suite (no regressions)**

Run: `pio test -e native`
Expected: PASS across all listed tests.

- [ ] **Step 4: Commit**

```bash
git add src/marker_math.cpp
git commit -m "feat(markers): pure marker-math placement + glyph-token map (green)"
```

---

## Task 3: `ui_markers` — Glyph drawing module (interface + glyph shapes)

**Files:**
- Create: `include/ui_markers.h`
- Create: `src/ui/ui_markers.cpp`

`draw_glyph` builds one marker visual on a 28×28 ARGB8888 `lv_canvas` (buffer in PSRAM via `heap_caps_malloc`, `MALLOC_CAP_SPIRAM`). Each glyph is drawn from a shape table in a unit box; `filled` selects a filled draw task vs. a stroked outline. Colors come from the passed `color` (already a theme token in the caller — no inline literals here).

Shape table (28×28 box, margin 4, so the live area is 4..24):

| Glyph | Geometry (points, px in 28-box) |
|---|---|
| Triangle | apex (14,4), base (4,24)&(24,24) — points up; ring places it pointing inward |
| Diamond | (14,4)(24,14)(14,24)(4,14) |
| Circle | center (14,14) r=10 |
| Bar | rect x 12..16, y 4..24 |
| Cross | rect 12..16×4..24 + rect 4..24×12..16 |
| ChevronIn | two lines (4,4)->(14,16)->(24,4) (V pointing down/in) |
| ChevronOut | two lines (4,24)->(14,12)->(24,24) (^ pointing up/out) |
| ChevronLeft | two lines (18,4)->(8,14)->(18,24) (`<`) |
| ChevronRight | two lines (10,4)->(20,14)->(10,24) (`>`) |
| ChevronDouble | ChevronLeft + ChevronRight offset ±5 in x |

- [ ] **Step 1: Write the header**

`include/ui_markers.h`:

```cpp
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "marker_math.h"  // ui::GlyphId, kMaxMarkersPerDial, placement math

// Shared marker-ring primitive for every compass-like widget (the semicircular
// autopilot HUD, the round Compass tile, the wind rose / wind-steer dials). One
// glyph set, one placement contract (screen_angle = value - reference). All
// colors are passed in from ui::theme by the caller (no inline magic here).

namespace ui {

using Glyph = GlyphId;  // the firmware enum is the pure-math enum

// One marker's presentation + current bearing. value_deg is refreshed each
// frame from the screen's data snapshot; NaN hides the marker.
struct MarkerSpec {
    double value_deg;  // current bearing (degrees, 0=N); NaN -> hidden
    Glyph glyph;
    bool filled;
    uint32_t color;  // resolved theme token
};

// An orbiting ring of up to kMaxMarkersPerDial markers, concentric with a dial
// of radius `r` centered at local (cx,cy) in `parent`. Each marker is a holder
// pivoting at the dial center; rotating it sweeps the glyph around the rim
// pointing inward. occlude_lower hides markers in the bottom half (semicircle).
struct MarkerRing {
    lv_obj_t *holder[kMaxMarkersPerDial];
    int16_t last_rot[kMaxMarkersPerDial];
    int8_t last_hidden[kMaxMarkersPerDial];
    uint8_t count;
    bool occlude_lower;
    int r;
};

// Build a 28x28 glyph visual (filled or outline) on a PSRAM canvas. Exposed for
// reuse (e.g. a legend); the ring uses it internally.
lv_obj_t *draw_glyph(lv_obj_t *parent, Glyph g, bool filled, uint32_t color);

// Build the ring. specs[].glyph/filled/color are fixed at build; specs[].value_deg
// is read later by marker_ring_update. count is clamped to kMaxMarkersPerDial.
MarkerRing build_marker_ring(lv_obj_t *parent, int cx, int cy, int r, const MarkerSpec *specs,
                             uint8_t count, bool occlude_lower);

// Place each marker at marker_screen_angle(specs[i].value_deg, reference_deg);
// hide NaN/occluded. Cheap: only changed rotations/visibility touch LVGL.
void marker_ring_update(MarkerRing &ring, const MarkerSpec *specs, uint8_t count,
                        double reference_deg);

}  // namespace ui
```

- [ ] **Step 2: Write the implementation**

`src/ui/ui_markers.cpp`:

```cpp
#include "ui_markers.h"

#include <esp_heap_caps.h>
#include <math.h>

#include "ui_dirty.h"

namespace ui {

static constexpr int GBOX = 28;  // glyph canvas side
static constexpr int MARGIN = 18;  // holder extends this far past the rim

// One marker holder: a transparent square concentric with the dial, pivoting at
// its center, carrying a glyph canvas at its top edge (pointing inward).
static lv_obj_t *make_holder(lv_obj_t *parent, int cx, int cy, int r) {
    int side = 2 * r + 2 * MARGIN;
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_set_size(h, side, side);
    lv_obj_set_pos(h, cx - side / 2, cy - side / 2);
    lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(h, side / 2, 0);
    lv_obj_set_style_transform_pivot_y(h, side / 2, 0);
    return h;
}

// Draw a glyph onto a freshly-created PSRAM canvas child of `parent`.
lv_obj_t *draw_glyph(lv_obj_t *parent, Glyph g, bool filled, uint32_t color) {
    uint32_t *buf = (uint32_t *)heap_caps_malloc(GBOX * GBOX * 4, MALLOC_CAP_SPIRAM);
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, GBOX, GBOX, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_set_size(cv, GBOX, GBOX);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_color_t col = lv_color_hex(color);

    // Shape points in the 28-box (see plan shape table).
    auto line = [&](int x0, int y0, int x1, int y1) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = col;
        d.width = 3;
        d.round_start = d.round_end = 1;
        d.p1.x = x0; d.p1.y = y0; d.p2.x = x1; d.p2.y = y1;
        lv_draw_line(&layer, &d);
    };
    auto tri = [&](lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c) {
        if (filled) {
            lv_draw_triangle_dsc_t d;
            lv_draw_triangle_dsc_init(&d);
            d.color = col; d.opa = LV_OPA_COVER;
            d.p[0] = a; d.p[1] = b; d.p[2] = c;
            lv_draw_triangle(&layer, &d);
        } else {
            line(a.x, a.y, b.x, b.y);
            line(b.x, b.y, c.x, c.y);
            line(c.x, c.y, a.x, a.y);
        }
    };

    switch (g) {
    case Glyph::Triangle:
        tri({14, 4}, {4, 24}, {24, 24});
        break;
    case Glyph::Diamond:
        if (filled) {
            tri({14, 4}, {24, 14}, {14, 24});
            tri({14, 4}, {4, 14}, {14, 24});
        } else {
            line(14, 4, 24, 14); line(24, 14, 14, 24);
            line(14, 24, 4, 14); line(4, 14, 14, 4);
        }
        break;
    case Glyph::Circle: {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = col;
        d.center.x = 14; d.center.y = 14;
        d.radius = 10;
        d.start_angle = 0; d.end_angle = 360;
        d.width = filled ? 10 : 3;  // wide stroke ~ filled disc
        lv_draw_arc(&layer, &d);
        break;
    }
    case Glyph::Bar: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = col; d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
        d.border_color = col; d.border_width = filled ? 0 : 2;
        lv_area_t a = {12, 4, 16, 24};
        lv_draw_rect(&layer, &d, &a);
        break;
    }
    case Glyph::Cross: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = col; d.bg_opa = LV_OPA_COVER;
        lv_area_t v = {12, 4, 16, 24};
        lv_area_t hh = {4, 12, 24, 16};
        lv_draw_rect(&layer, &d, &v);
        lv_draw_rect(&layer, &d, &hh);
        break;
    }
    case Glyph::ChevronIn:
        line(4, 4, 14, 16); line(14, 16, 24, 4);
        break;
    case Glyph::ChevronOut:
        line(4, 24, 14, 12); line(14, 12, 24, 24);
        break;
    case Glyph::ChevronLeft:
        line(18, 4, 8, 14); line(8, 14, 18, 24);
        break;
    case Glyph::ChevronRight:
        line(10, 4, 20, 14); line(20, 14, 10, 24);
        break;
    case Glyph::ChevronDouble:
        line(13, 4, 3, 14); line(3, 14, 13, 24);     // left half
        line(25, 4, 15, 14); line(15, 14, 25, 24);   // right half
        break;
    default:
        break;
    }

    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

MarkerRing build_marker_ring(lv_obj_t *parent, int cx, int cy, int r, const MarkerSpec *specs,
                             uint8_t count, bool occlude_lower) {
    MarkerRing ring = {};
    ring.r = r;
    ring.occlude_lower = occlude_lower;
    if (count > kMaxMarkersPerDial) count = kMaxMarkersPerDial;
    ring.count = count;
    for (uint8_t i = 0; i < count; ++i) {
        lv_obj_t *h = make_holder(parent, cx, cy, r);
        lv_obj_t *cv = draw_glyph(h, specs[i].glyph, specs[i].filled, specs[i].color);
        // Sit the glyph at the holder's top edge, centered, just outside the rim.
        lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, MARGIN - GBOX - 2);
        lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);  // shown on first update
        ring.holder[i] = h;
        ring.last_rot[i] = INT16_MIN;
        ring.last_hidden[i] = -1;
    }
    return ring;
}

void marker_ring_update(MarkerRing &ring, const MarkerSpec *specs, uint8_t count,
                        double reference_deg) {
    if (count > ring.count) count = ring.count;
    double half = ring.occlude_lower ? 96.0 : 360.0;
    for (uint8_t i = 0; i < count; ++i) {
        double sa = marker_screen_angle(specs[i].value_deg, reference_deg);
        bool hide = isnan(sa) || (ring.occlude_lower && marker_occluded(sa, half));
        set_hidden_if_changed(ring.holder[i], &ring.last_hidden[i], hide);
        if (hide) continue;
        int16_t rot = (int16_t)(lround(sa) * 10 % 3600);
        set_rot_if_changed(ring.holder[i], &ring.last_rot[i], rot);
    }
}

}  // namespace ui
```

- [ ] **Step 3: Add `ui_markers.cpp` to the production build and confirm it compiles**

The production env `[env:esp32-4848s040]` uses a default `+<*>` src filter (it compiles all of `src/`), so no edit is needed there. Build it:

Run: `pio run -e esp32-4848s040`
Expected: SUCCESS (new TU compiles; nothing references it yet).

- [ ] **Step 4: Commit**

```bash
git add include/ui_markers.h src/ui/ui_markers.cpp
git commit -m "feat(markers): LVGL glyph drawing + orbiting MarkerRing primitive"
```

---

## Task 4: Wire the steering tile Compass to `MarkerRing`

**Files:**
- Modify: `src/ui/ui_layouts.cpp` (struct `QuadGridTile`, `paint_compass_body` ~458-518, `update_quad_grid` Compass case ~986-1008)

The steering tile is the round (north-up bezel) Compass. Default markers: HDG ▲filled (accent), COG △hollow (good), CTS ◆filled (warn). Reference = the center value's bearing (HDG), so it becomes value-up. The existing single `aux2` needle is removed in favor of the ring; the CTS text line stays.

- [ ] **Step 1: Read the current tile struct + compass paths**

Read `src/ui/ui_layouts.cpp` lines 30-50 (the `QuadGridTile` struct) and confirm field names (`root`, `aux`, `aux2`, `value`, `secondary`, `last_*`). Add a `MarkerRing markers;` field to `QuadGridTile` and `#include "ui_markers.h"` at the top of the file.

- [ ] **Step 2: Replace the single marker in `paint_compass_body`**

In `paint_compass_body`, remove the line `t.aux2 = make_dir_marker(t.root, dia, 4, theme.accent);` and after the ring object `t.aux` is created (the bezel), build the marker ring concentric with it. The ring center in tile-local coords is the tile center offset by the ring's `+4` y-align; use the tile geometry already in scope (`w`,`h`):

```cpp
// Default steering marker set: HDG (solid triangle), COG (hollow triangle),
// CTS (solid diamond). Bearings filled live in update from metric_scalar().
static const MarkerSpec kSteerMarkers[3] = {
    {NAN, Glyph::Triangle, true, theme.accent},
    {NAN, Glyph::Triangle, false, theme.good},
    {NAN, Glyph::Diamond, true, theme.warn},
};
int rr = dia / 2;
t.markers = build_marker_ring(t.root, w / 2, h / 2 + 4, rr, kSteerMarkers, 3,
                              /*occlude_lower=*/false);
```

(Place this right after `t.aux = ring;`. `w`,`h` are the `paint_compass_body` params; the ring concentric with the bezel which is `LV_ALIGN_CENTER, 0, 4`.)

- [ ] **Step 3: Drive the ring in the Compass update case**

In `update_quad_grid`, Compass case (~1006), replace `rotate_needle(t, metric_scalar(m, data));` with:

```cpp
// Markers: HDG/COG/CTS bearings; reference = HDG so the dial is heading-up.
MarkerBinding hdg = {}, cog = {}, cts = {};
hdg.source = MetricSource::HDG_deg;
cog.source = MetricSource::COG_deg;
cts.source = MetricSource::CTS_deg;
MarkerSpec live[3] = {
    {metric_scalar(hdg, data), Glyph::Triangle, true, theme.accent},
    {metric_scalar(cog, data), Glyph::Triangle, false, theme.good},
    {metric_scalar(cts, data), Glyph::Diamond, true, theme.warn},
};
double ref = metric_scalar(hdg, data);
if (isnan(ref)) ref = 0.0;  // no heading -> north-up
marker_ring_update(t.markers, live, 3, ref);
```

`MarkerBinding` here is just a local `MetricBinding`; reuse `MetricBinding` (rename the snippet's type to `MetricBinding`). `metric_scalar` already returns HDG/COG/CTS in degrees (NaN when absent).

- [ ] **Step 4: Build for the device**

Run: `pio run -e esp32-4848s040`
Expected: SUCCESS.

- [ ] **Step 5: Sim-render the steering screen**

Run: `tools/sim_render.sh` (or the project's documented single-size invocation, e.g. `pio run -e sim-screens` then open the BMP). Confirm the steering tile shows three distinct markers (solid cyan triangle, hollow green triangle, solid amber diamond) with HDG at top.

- [ ] **Step 6: Commit**

```bash
git add src/ui/ui_layouts.cpp
git commit -m "feat(markers): steering tile compass shows HDG/COG/CTS marker ring"
```

---

## Task 5: Wire the Autopilot HUD semicircular compass

**Files:**
- Modify: `include/ui_compass.h` (add a `MarkerRing` to `Compass`)
- Modify: `src/ui/ui_compass.cpp` (`build_compass` — build the ring; keep the lubber)
- Modify: `src/ui/screen_autopilot.cpp` (`refresh` — drive HDG/COG/CTS + target)

Keep the fixed red lubber (top reference). Replace the single amber `bug` with a `MarkerRing` of HDG/COG/CTS + an optional 4th AP-target marker (amber bug, shown only when a target exists). Reference = HDG (heading-up); `occlude_lower = true`.

- [ ] **Step 1: Extend the `Compass` struct + header include**

In `include/ui_compass.h` add `#include "ui_markers.h"` and a field `MarkerRing markers;` to `struct Compass`. Leave `bug` in place for now (the target marker migrates into the ring; remove `bug` once Step 3 compiles).

- [ ] **Step 2: Build the ring in `build_compass`**

In `src/ui/ui_compass.cpp` `build_compass`, after the lubber is created, build a 4-marker ring concentric with the arc (`cx,cy`,`r`). The target marker (index 3) starts hidden:

```cpp
static const MarkerSpec kApMarkers[4] = {
    {NAN, Glyph::Triangle, true, theme.accent},   // HDG
    {NAN, Glyph::Triangle, false, theme.good},    // COG
    {NAN, Glyph::Diamond, true, theme.alarm},     // CTS
    {NAN, Glyph::Diamond, true, theme.warn},      // AP target (amber bug)
};
cp.markers = build_marker_ring(root, cx, cy, r, kApMarkers, 4, /*occlude_lower=*/true);
```

Delete the old `bug` holder/label block and the `cp.bug = bug;` line (and remove `bug` from the struct).

- [ ] **Step 3: Drive markers in `screen_autopilot.cpp::refresh`**

Replace the target-bug block (lines ~379-387) and remove the old `s_last_bug_rot`/`s_last_bug_hidden` statics. Build the live spec array each refresh (HDG/COG/CTS from `d`, target from `s_target_local`/`d.apTargetHdg`):

```cpp
double hdg = isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
double cog = isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
double cts = isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
double tgt = !isnan(s_target_local) ? rad_to_deg_pos(s_target_local)
                                     : (isnan(d.apTargetHdg) ? NAN : rad_to_deg_pos(d.apTargetHdg));
ui::MarkerSpec live[4] = {
    {hdg, ui::Glyph::Triangle, true, theme.accent},
    {cog, ui::Glyph::Triangle, false, theme.good},
    {cts, ui::Glyph::Diamond, true, theme.alarm},
    {tgt, ui::Glyph::Diamond, true, theme.warn},
};
double ref = isnan(hdg) ? 0.0 : hdg;
ui::marker_ring_update(s_cp.markers, live, 4, ref);
```

(The HDG marker at offset 0 sits under the red lubber — exactly today's reference. The COG sub-line text stays unchanged.)

- [ ] **Step 4: Build for the device**

Run: `pio run -e esp32-4848s040`
Expected: SUCCESS (no remaining references to `s_cp.bug` / `s_last_bug_*`).

- [ ] **Step 5: Sim-render the AP HUD at 480/800/1024**

Run: the AP sim (`tools/sim_render.sh` AP path, or `pio run -e sim-ap`). Confirm HDG under the lubber, COG/CTS bugs at their relative bearings, amber target bug only when a target is set, lower-half markers hidden.

- [ ] **Step 6: Commit**

```bash
git add include/ui_compass.h src/ui/ui_compass.cpp src/ui/screen_autopilot.cpp
git commit -m "feat(markers): autopilot HUD shows HDG/COG/CTS + target marker ring"
```

---

## Task 6: Wire the wind rose tile + wind-steer screen

**Files:**
- Modify: `src/ui/ui_layouts.cpp` (`paint_wind_rose_body` ~613-644, WindRose update case ~1017-1022)
- Modify: `src/ui/screen_wind_steer.cpp` (read in full first; wire like the AP HUD)

The wind rose center value is a speed (AWS), so `reference` falls back to a configured angle. Use **north (0)** for the rose tile (north-up) and the wind-steer screen's existing heading reference for that screen. Default rose markers: apparent wind ChevronIn (warn) + true wind ChevronOut (good).

- [ ] **Step 1: Replace the rose's single needle with a ring**

In `paint_wind_rose_body`, remove `t.aux2 = make_dir_marker(t.root, dia, 4, theme.warn);` and build:

```cpp
static const MarkerSpec kWindMarkers[2] = {
    {NAN, Glyph::ChevronIn, true, theme.warn},   // apparent wind
    {NAN, Glyph::ChevronOut, false, theme.good},  // true wind
};
int rr = dia / 2;
t.markers = build_marker_ring(t.root, w / 2, h / 2 + 4, rr, kWindMarkers, 2,
                              /*occlude_lower=*/false);
```

- [ ] **Step 2: Drive the rose markers**

In the `update_quad_grid` default case WindRose branch (~1017-1022), replace the single `rotate_needle(t, metric_scalar(awa, data));` with:

```cpp
MetricBinding awa = {}, twa = {};
awa.source = MetricSource::AWA_deg;
twa.source = MetricSource::TWA_deg;
MarkerSpec wind[2] = {
    {metric_scalar(awa, data), Glyph::ChevronIn, true, theme.warn},
    {metric_scalar(twa, data), Glyph::ChevronOut, false, theme.good},
};
marker_ring_update(t.markers, wind, 2, 0.0);  // north-up rose
```

- [ ] **Step 3: Wind-steer screen**

Read `src/ui/screen_wind_steer.cpp` in full. It mirrors the AP HUD geometry. If it uses `ui::build_compass` it already gained the ring in Task 5 — drive it in its `refresh` exactly as in Task 5 Step 3 (HDG/COG/CTS + optionally a wind-target marker), reference = HDG. If it hand-rolls its own dial, add a `MarkerRing` field the same way and feed AWA/TWA/HDG markers, reference = HDG. Keep the existing no-go / target sectors untouched.

- [ ] **Step 4: Build + sim-render**

Run: `pio run -e esp32-4848s040` (Expected: SUCCESS), then `pio run -e sim-wind` and the wind-steer sim. Confirm apparent/true chevrons orbit correctly and the wind-steer dial shows its markers.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_layouts.cpp src/ui/screen_wind_steer.cpp
git commit -m "feat(markers): wind rose + wind-steer adopt marker ring (apparent/true heads)"
```

---

## Task 7: Remove the dead `make_dir_marker` + `rotate_needle` if unused

**Files:**
- Modify: `src/ui/ui_layouts.cpp`

- [ ] **Step 1: Check for remaining callers**

Run: `grep -n "make_dir_marker\|rotate_needle" src/ui/ui_layouts.cpp`
Expected: only the definitions remain (Compass + WindRose now use the ring). If the zoom screen (`metric_scalar` at line ~1616) still uses `rotate_needle` for a zoomed compass, leave `rotate_needle` and skip its removal.

- [ ] **Step 2: Delete whichever helper has zero callers**

Remove `static lv_obj_t *make_dir_marker(...)` (now unused) and, only if it has no callers, `rotate_needle`. Leave `t.aux2`/`last_aux_pct` in the struct (still used by gauge/bar).

- [ ] **Step 3: Build + native tests**

Run: `pio run -e esp32-4848s040` (SUCCESS) and `pio test -e native` (PASS).

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_layouts.cpp
git commit -m "refactor(markers): drop unused single-marker helper"
```

---

## Task 8: Report the manifest (glyphs + maxMarkersPerDial)

**Files:**
- Modify: `src/capabilities.cpp`
- Modify: `test/test_capabilities/test_capabilities.cpp`

- [ ] **Step 1: Read the current manifest builder**

Read `src/capabilities.cpp` and `include/capabilities.h` (or wherever the manifest JSON/struct is built) to find where `maxViews` / `maxTilesPerScreen` are emitted.

- [ ] **Step 2: Write the failing test**

In `test/test_capabilities/test_capabilities.cpp`, add a test asserting the emitted manifest contains `maxMarkersPerDial == 12` and a `glyphs` array including `"triangle"`, `"diamond"`, `"chevron_in"`, `"chevron_double"` (match the existing test's parse/access style in that file).

- [ ] **Step 3: Run it red**

Run: `pio test -e native -f test_capabilities`
Expected: FAIL (fields absent).

- [ ] **Step 4: Emit the fields**

Next to `maxTilesPerScreen`, emit `maxMarkersPerDial = ui::kMaxMarkersPerDial` (include `marker_math.h`) and a `glyphs` array built from `ui::glyph_to_token((GlyphId)i)` for `i in [0, GlyphId::COUNT)`. Match the file's JSON-building idiom.

- [ ] **Step 5: Run it green + full suite**

Run: `pio test -e native -f test_capabilities` (PASS), then `pio test -e native` (PASS).

- [ ] **Step 6: Commit**

```bash
git add src/capabilities.cpp test/test_capabilities/test_capabilities.cpp
git commit -m "feat(markers): report glyphs + maxMarkersPerDial in capability manifest"
```

---

## Task 9: Sim previews, lint, and docs

**Files:**
- Modify: `platformio.ini` (`sim*` envs that render a compass — add `+<ui/ui_markers.cpp>` and `+<marker_math.cpp>` to their `build_src_filter` if those envs don't already `+<*>`)
- Modify: `docs/widget-previews/*`, `README.md` gallery if it references the compass/steering tiles

- [ ] **Step 1: Ensure sim envs compile the new TUs**

For each of `[env:sim]`, `[env:sim-ap]`, `[env:sim-wind]`, `[env:sim-screens]` whose `build_src_filter` is an explicit allow-list (not `+<*>`), append ` +<ui/ui_markers.cpp> +<marker_math.cpp>`. Run each: `pio run -e sim-ap` / `-e sim-wind` / `-e sim-screens`.
Expected: SUCCESS.

- [ ] **Step 2: Regenerate previews**

Run: `tools/sim_render.sh` (its documented full invocation) to regenerate the steering / AP / wind preview PNGs at 480/800/1024. Visually confirm markers.

- [ ] **Step 3: Lint**

Run: `make pre-commit`
Expected: PASS (clang-format clean). Note `board_pins.h` clang-format guards are untouched.

- [ ] **Step 4: Full gate**

Run: `pio test -e native` (PASS) and `pio run -e esp32-4848s040` (SUCCESS).

- [ ] **Step 5: Commit**

```bash
git add platformio.ini docs/ README.md
git commit -m "docs(markers): regenerate compass/steering/wind previews with marker rings"
```

---

## Task 10: Device verification (hardware)

**Files:** none

- [ ] **Step 1: OTA flash + confirm boot**

Run: `make ota-verify DEVICE_IP=<lab device>` (or the lab device per project memory). Confirm the new binary boots (no silent reboot — the `MarkerRing` holders are LVGL objects on the UI task, canvases in PSRAM; verify no internal-heap starvation in logs).

- [ ] **Step 2: Eyeball the steering + AP + wind screens on the panel**

Switch to the steering screen, AP HUD, and wind screens; confirm HDG/COG/CTS markers track live data and the AP target bug appears only when engaged with a target.

- [ ] **Step 3: Record result**

Note the verified firmware in the final summary. No commit.

---

## Self-Review

- **Spec coverage:** marker model + reference-relative placement (Tasks 1-5), glyph set hollow/filled (Task 3), steering/AP/wind wiring (Tasks 4-6), manifest glyphs + cap (Task 8), tests + sim (Tasks 1-2, 9), device verify (Task 10). Manager previews + authored-config parsing are explicitly deferred (scope note) per the spec's editor slice. Dynamic marker sources are documented follow-on (not in this plan).
- **Types consistent:** `GlyphId`/`Glyph` (alias), `MarkerSpec{value_deg,glyph,filled,color}`, `MarkerRing`, `build_marker_ring`/`marker_ring_update`, `kMaxMarkersPerDial=12` used identically across header, impl, wiring, and manifest. `metric_scalar()` reused for bearings (returns degrees, NaN-safe).
- **No placeholders:** every code step shows full code; sim/`tools/sim_render.sh` invocations reference the existing harness (Task 9 adjusts filters where needed).
- **Memory traps:** no large stack structs; canvases in PSRAM; refresh keeps `sk::copyData` snapshot pattern; LVGL mutations only on the UI/refresh path. Files touched are not in the `out=Config{}` trap set, but the no-large-stack-temporary rule is honored.
