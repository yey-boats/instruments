// Host tests for include/round_safe.h (instruments#50): the geometry cases
// mirror midl web/test/round-safe.test.ts with the same numeric expectations,
// plus the firmware tile plan used by build_tile on round panels.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <math.h>
#include <unity.h>

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
        for (int dial = 0; dial < 2; ++dial) {
            const TilePlan p = plan_tile(c, d, dial != 0, 3);
            TEST_ASSERT_TRUE(p.hidden);
            TEST_ASSERT_TRUE(p.cap.w >= 0 && p.cap.h >= 0);
            TEST_ASSERT_TRUE(p.body.w >= 0 && p.body.h >= 0);
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

// The 2x2 grid on round-360: every quadrant's content lands inside the disc
// and inside its own cell; a dial gets a square body clear of the caption.
static void test_quadrants_plan_inside_disc() {
    const Disc d = viewport_disc(VP);
    const IRect cells[] = {
        {0, 0, 180, 180}, {180, 0, 180, 180}, {0, 180, 180, 180}, {180, 180, 180, 180}};
    for (const IRect &c : cells) {
        for (int dial = 0; dial < 2; ++dial) {
            const TilePlan p = plan_tile(c, d, dial != 0, 3);
            TEST_ASSERT_FALSE(p.hidden);
            TEST_ASSERT_TRUE(irect_in_disc(p.cap, d));
            TEST_ASSERT_TRUE(irect_within(p.cap, c));
            TEST_ASSERT_TRUE(irect_within(p.body, c));
            if (!dial) {
                TEST_ASSERT_TRUE(irect_in_disc(p.body, d));
            } else {
                // the body is the square circumscribing the fitted circle: the
                // circle (not the square's corners) lies inside the disc
                const double r = p.body.w / 2.0;
                const double cx = p.body.x + r, cy = p.body.y + r;
                TEST_ASSERT_TRUE(hypot(cx - d.cx, cy - d.cy) + r <= d.r + 0.5);
                TEST_ASSERT_EQUAL_INT(p.body.w, p.body.h);
                TEST_ASSERT_TRUE(p.body.w > 40);
            }
        }
    }
}

// A full-screen non-dial tile shrinks to (about) the inscribed square; a
// full-screen dial keeps (about) the whole disc.
static void test_full_screen_tile_plan() {
    const Disc d = viewport_disc(VP);  // r = 176
    const TilePlan num = plan_tile({0, 0, 360, 360}, d, false, 3);
    TEST_ASSERT_FALSE(num.hidden);
    TEST_ASSERT_TRUE(irect_in_disc(num.cap, d));
    TEST_ASSERT_INT_WITHIN(3, 248, num.cap.w);  // 176 * sqrt(2)
    const TilePlan dial = plan_tile({0, 0, 360, 360}, d, true, 0);
    TEST_ASSERT_TRUE(dial.body.w >= 348);
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
    RUN_TEST(test_full_screen_tile_plan);
    RUN_TEST(test_inner_pixels_shrinks_inward);
    return UNITY_END();
}
