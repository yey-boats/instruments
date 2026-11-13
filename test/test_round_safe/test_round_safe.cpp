// Host tests for include/round_safe.h (instruments#50): the geometry cases
// mirror midl web/test/round-safe.test.ts with the same numeric expectations,
// plus the firmware tile plan used by build_tile on round panels.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <ArduinoJson.h>

#include "midl_demo_doc.h"
#include "midl_solve.h"
#include "round_safe.h"

using namespace ui::round_safe;

void setUp() {
}
void tearDown() {
}

static const Rect VP = {0, 0, 360, 360};

// ---- midl "round-safe geometry" ------------------------------------------

static void test_cell_inside_disc_unchanged() {
    const Disc d = viewport_disc(VP, 0);
    const Rect t = {100, 100, 160, 160};
    const Rect s = safe_rect(t, d);
    TEST_ASSERT_EQUAL_DOUBLE(t.x, s.x);
    TEST_ASSERT_EQUAL_DOUBLE(t.y, s.y);
    TEST_ASSERT_EQUAL_DOUBLE(t.w, s.w);
    TEST_ASSERT_EQUAL_DOUBLE(t.h, s.h);
}

static void test_corner_quadrant_shrinks_inside_disc() {
    const Disc d = viewport_disc(VP, 0);
    const Rect s = safe_rect({180, 180, 180, 180}, d);
    TEST_ASSERT_TRUE(rect_in_disc(s, d));
    TEST_ASSERT_TRUE(s.w * s.h > 0.45 * 180 * 180);  // r²/2 is the optimum
}

static void test_full_screen_keeps_full_size_dial() {
    const Disc d = viewport_disc(VP, 0);
    const Rect c = fit_circle({0, 0, 360, 360}, d);
    TEST_ASSERT_TRUE(c.w > 355);
}

static void test_fit_circle_avoids_caption_box() {
    const Disc d = viewport_disc(VP, 0);
    const Rect box = {60, 60, 60, 30};
    const Rect c = fit_circle({0, 0, 360, 180}, d, &box);
    const double cx = c.x + c.w / 2, cy = c.y + c.h / 2, r = c.w / 2;
    const double dx = fmax(fmax(box.x - cx, 0), cx - box.x - box.w);
    const double dy = fmax(fmax(box.y - cy, 0), cy - box.y - box.h);
    TEST_ASSERT_TRUE(hypot(dx, dy) >= r - 0.01);
}

static const Dial kDials[] = {Dial::None, Dial::Compass, Dial::WindRose, Dial::Gauge};

// ---- midl "round-safe degenerate cells" ----------------------------------

static void test_viewport_disc_never_negative() {
    TEST_ASSERT_EQUAL_DOUBLE(0, viewport_disc({0, 0, 4, 4}).r);
}

// midl: a cell entirely under the bezel draws only its panel, never negative
// sizes (40x40 / 60x60 cells in the top-left corner of a 360 round panel).
static void test_corner_cell_under_bezel_is_panel_only() {
    const Disc d = viewport_disc(VP);
    const IRect cells[] = {{0, 0, 40, 40}, {0, 0, 60, 60}};
    for (const IRect &c : cells) {
        for (Dial dial : kDials) {
            const TilePlan p = plan_tile(c, d, dial, 3);
            TEST_ASSERT_TRUE(p.hidden);
            TEST_ASSERT_TRUE(p.cap.w >= 0 && p.cap.h >= 0);
            TEST_ASSERT_TRUE(p.body.w >= 0 && p.body.h >= 0);
            TEST_ASSERT_EQUAL_INT(0, p.ring_dia);
            TEST_ASSERT_FALSE(p.value_only);
        }
    }
    const Rect s = safe_rect({0, 0, 40, 40}, d);
    TEST_ASSERT_TRUE(s.w >= 0 && s.h >= 0);
    const Rect c = fit_circle({0, 0, 40, 40}, d);
    TEST_ASSERT_TRUE(c.w >= 0 && c.h >= 0);
}

// ---- firmware tile plan --------------------------------------------------

static bool irect_in_disc(const IRect &r, const Disc &d) {
    return rect_in_disc({(double)r.x, (double)r.y, (double)r.w, (double)r.h}, d);
}
static bool irect_within(const IRect &in, const IRect &out) {
    return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
           in.y + in.h <= out.y + out.h;
}

// Distance from a point to an integer box (0 inside it).
static double dist_to_irect(double px, double py, const IRect &b) {
    const double dx = fmax(fmax(b.x - px, 0), px - (b.x + b.w));
    const double dy = fmax(fmax(b.y - py, 0), py - (b.y + b.h));
    return hypot(dx, dy);
}

// Everything a tile draws on a round panel stays in its own cell, inside the
// visible disc and clear of its caption:
//   - caption + rect content (and the value-only fallback) in the safe rect;
//   - a ring dial (ring + marker / zone-band overhang) inside the fitted
//     circle, which is concentric with the ring the painter box produces.
static void assert_plan_contained(const IRect &cell, const Disc &d, Dial dial, size_t cap_len,
                                  const char *msg) {
    const TilePlan p = plan_tile(cell, d, dial, cap_len);
    TEST_ASSERT_FALSE_MESSAGE(p.hidden, msg);
    TEST_ASSERT_TRUE_MESSAGE(irect_within(p.cap, cell), msg);
    TEST_ASSERT_TRUE_MESSAGE(irect_in_disc(p.cap, d), msg);
    if (p.ring_dia == 0) {
        // rect content: the painter box is the safe rect itself
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&p.cap, &p.body, sizeof(IRect), msg);
        // a dial without a ring is the value-only fallback, and only a dial is
        TEST_ASSERT_TRUE_MESSAGE(p.value_only == (dial != Dial::None), msg);
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(dial != Dial::None, msg);
    TEST_ASSERT_FALSE_MESSAGE(p.value_only, msg);
    // never below the painter's own minimum (that is the value-only case)
    TEST_ASSERT_TRUE_MESSAGE(p.ring_dia >= dial_min_ring(dial), msg);
    // ring + overhang fits the fitted circle
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.circle.w, p.circle.h, msg);
    TEST_ASSERT_TRUE_MESSAGE(p.ring_dia + 2 * dial_overhang(dial) <= p.circle.w, msg);
    // the fitted circle is in the cell, in the disc and clear of the caption
    TEST_ASSERT_TRUE_MESSAGE(irect_within(p.circle, cell), msg);
    const double r = p.circle.w / 2.0;
    const double cx = p.circle.x + r, cy = p.circle.y + r;
    TEST_ASSERT_TRUE_MESSAGE(hypot(cx - d.cx, cy - d.cy) + r <= d.r + 0.5, msg);
    if (cap_len) {
        const IRect box = caption_box(p.cap, cap_len);
        TEST_ASSERT_TRUE_MESSAGE(dist_to_irect(cx, cy, box) >= r - 0.5, msg);
    }
    // the painter box draws exactly ring_dia, concentric with the circle: the
    // ring painters use dia = min(w, h) - kDialReserve, kDialDrop below centre
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.body.w, p.body.h, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.ring_dia, p.body.w - kDialReserve, msg);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, cx, p.body.x + p.body.w / 2.0, msg);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, cy, p.body.y + p.body.h / 2.0 + kDialDrop, msg);
}

// The 2x2 grid on round-360: every quadrant's content lands inside the disc
// and inside its own cell, for every caption length.
static void test_quadrants_plan_inside_disc() {
    const Disc d = viewport_disc(VP);
    const IRect cells[] = {
        {0, 0, 180, 180}, {180, 0, 180, 180}, {0, 180, 180, 180}, {180, 180, 180, 180}};
    for (const IRect &c : cells)
        for (Dial dial : kDials)
            for (size_t len = 0; len <= 12; ++len) {
                char msg[96];
                snprintf(msg, sizeof(msg), "cell %d,%d dial %d caption %u", c.x, c.y, (int)dial,
                         (unsigned)len);
                assert_plan_contained(c, d, dial, len, msg);
            }
}

// A ring is only ever planned when it fits: sweep cell sizes and positions.
static void test_ring_never_exceeds_fitted_circle() {
    const Disc d = viewport_disc(VP);
    for (int w = 60; w <= 360; w += 20)
        for (int h = 60; h <= 360; h += 20)
            for (int x = 0; x + w <= 360; x += 60)
                for (int y = 0; y + h <= 360; y += 60)
                    for (Dial dial : kDials) {
                        const IRect c = {x, y, w, h};
                        const TilePlan p = plan_tile(c, d, dial, 5);
                        if (p.hidden) continue;
                        char msg[96];
                        snprintf(msg, sizeof(msg), "cell %d,%d %dx%d dial %d", x, y, w, h,
                                 (int)dial);
                        assert_plan_contained(c, d, dial, 5, msg);
                    }
}

// The quadrant dials the shipped screens produce. Top-row circles (squeezed
// between the caption and the bezel) cannot hold a ring: value-only, where the
// painters used to draw their floor-size ring across the cell edge (88 px RPM /
// COOLANT gauges overlapping, PR #59 review). Bottom-row circles hold one.
static void test_small_circles_fall_back_to_value_only() {
    const Disc d = viewport_disc(VP);
    const TilePlan rpm = plan_tile({0, 0, 180, 180}, d, Dial::Gauge, strlen("RPM"));
    const TilePlan coolant = plan_tile({180, 0, 180, 180}, d, Dial::Gauge, strlen("COOLANT"));
    const TilePlan heading = plan_tile({180, 0, 180, 180}, d, Dial::Compass, strlen("HEADING"));
    TEST_ASSERT_TRUE(rpm.value_only);
    TEST_ASSERT_TRUE(coolant.value_only);
    TEST_ASSERT_TRUE(heading.value_only);
    TEST_ASSERT_EQUAL_INT(0, rpm.ring_dia);
    const TilePlan hdg = plan_tile({180, 180, 180, 180}, d, Dial::Compass, strlen("HDG"));
    TEST_ASSERT_FALSE(hdg.value_only);
    TEST_ASSERT_TRUE(hdg.ring_dia >= dial_min_ring(Dial::Compass));
}

static Dial dial_of(JsonObjectConst el) {
    const char *type = el["type"] | "";
    if (!strcmp(type, "gauge")) return Dial::Gauge;
    if (!strcmp(type, "windrose")) return Dial::WindRose;
    // the band compass is not a ring: it lays out in the safe rect
    if (!strcmp(type, "compass"))
        return strcmp(el["style"]["shape"] | "", "band") ? Dial::Compass : Dial::None;
    return Dial::None;
}

// The shipped screens (the baked demo doc, rendered by every board) on both
// round panels: the 360 px knob and the 240 px touch LCD (which runs the
// round-360 class). No tile's content leaves its cell or the disc, and no two
// tiles' content regions meet.
static void test_shipped_round_layouts_contained() {
    JsonDocument doc;
    TEST_ASSERT_EQUAL(DeserializationError::Ok,
                      deserializeJson(doc, midl::demo::SQUARE_480_JSON).code());
    const int sizes[] = {360, 240};
    for (int size : sizes) {
        const Disc d = viewport_disc({0, 0, (double)size, (double)size});
        for (JsonObjectConst s : doc["screens"].as<JsonArrayConst>()) {
            midl::PlacementSet ps;
            TEST_ASSERT_EQUAL_INT(midl::SOLVE_OK,
                                  midl::solve_screen(s["layout"], {0, 0, size, size}, ps, 2, 4));
            TEST_ASSERT_TRUE(ps.count > 0);
            IRect content[midl::FirmwareLimits::max_tiles_per_screen];
            for (size_t i = 0; i < ps.count; ++i) {
                char msg[96];
                snprintf(msg, sizeof(msg), "%d px screen %s element %s", size, s["id"] | "?",
                         ps.items[i].element);
                JsonObjectConst el = s["elements"][(const char *)ps.items[i].element];
                TEST_ASSERT_FALSE_MESSAGE(el.isNull(), msg);
                const Dial dial = dial_of(el);
                const bool has_cap = strcmp(el["type"] | "", "button") != 0;
                const size_t cap_len = has_cap ? strlen(el["name"] | "") : 0;
                const midl::Rect &r = ps.items[i].rect;
                const IRect cell = {r.x, r.y, r.w, r.h};
                assert_plan_contained(cell, d, dial, cap_len, msg);
                const TilePlan p = plan_tile(cell, d, dial, cap_len);
                content[i] = p.ring_dia ? p.circle : p.cap;
                for (size_t j = 0; j < i; ++j) {
                    const IRect &a = content[i], &b = content[j];
                    const bool apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y ||
                                       b.y + b.h <= a.y;
                    TEST_ASSERT_TRUE_MESSAGE(apart, msg);
                }
            }
        }
    }
}

// A full-screen non-dial tile shrinks to (about) the inscribed square; a
// full-screen dial keeps (about) the whole disc.
static void test_full_screen_tile_plan() {
    const Disc d = viewport_disc(VP);  // r = 176
    const TilePlan num = plan_tile({0, 0, 360, 360}, d, Dial::None, 3);
    TEST_ASSERT_FALSE(num.hidden);
    TEST_ASSERT_TRUE(irect_in_disc(num.cap, d));
    TEST_ASSERT_INT_WITHIN(3, 248, num.cap.w);  // 176 * sqrt(2)
    const TilePlan dial = plan_tile({0, 0, 360, 360}, d, Dial::Compass, 0);
    TEST_ASSERT_TRUE(dial.circle.w >= 348);
    TEST_ASSERT_EQUAL_INT(dial.circle.w - 2 * dial_overhang(Dial::Compass), dial.ring_dia);
}

static void test_inner_pixels_shrinks_inward() {
    const IRect r = inner_pixels({10.2, 20.8, 30.5, 40.1});
    TEST_ASSERT_EQUAL_INT(11, r.x);
    TEST_ASSERT_EQUAL_INT(21, r.y);
    TEST_ASSERT_EQUAL_INT(40 - 11, r.w);  // floor(40.7) - 11
    TEST_ASSERT_EQUAL_INT(60 - 21, r.h);  // floor(60.9) - 21
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cell_inside_disc_unchanged);
    RUN_TEST(test_corner_quadrant_shrinks_inside_disc);
    RUN_TEST(test_full_screen_keeps_full_size_dial);
    RUN_TEST(test_fit_circle_avoids_caption_box);
    RUN_TEST(test_viewport_disc_never_negative);
    RUN_TEST(test_corner_cell_under_bezel_is_panel_only);
    RUN_TEST(test_quadrants_plan_inside_disc);
    RUN_TEST(test_ring_never_exceeds_fitted_circle);
    RUN_TEST(test_small_circles_fall_back_to_value_only);
    RUN_TEST(test_shipped_round_layouts_contained);
    RUN_TEST(test_full_screen_tile_plan);
    RUN_TEST(test_inner_pixels_shrinks_inward);
    return UNITY_END();
}
