// Host tests for include/ui_geom.h: compass legend placement, heading-band
// numeral hysteresis and the square wind-steer stack (PR #43 follow-ups).

#include <math.h>
#include <unity.h>

#include "ui_geom.h"

using namespace ui::geom;

void setUp() {
}
void tearDown() {
}

// Round compass tile geometry through the same helpers paint_compass_body uses
// (chrome inset 11 = panel_border 1 + panel_pad 10).
struct Tile {
    int cw, ch, mcx, mcy, dia;
};
static Tile compass_tile(int w, int h) {
    const int inset = 11;
    Tile t;
    t.cw = w - 2 * inset;
    t.ch = h - 2 * inset;
    dial_center(w, h, inset, t.mcx, t.mcy);
    t.dia = dial_ring_dia(w, h, 72);
    return t;
}
static bool legend_fits(int w, int h) {
    Tile t = compass_tile(w, h);
    return compass_legend_fits(t.cw, t.ch, t.mcx, t.mcy, t.dia);
}

static void test_box_clears_disc() {
    TEST_ASSERT_TRUE(box_clears_disc({0, 0, 10, 10}, 100, 100, 50, 0));
    TEST_ASSERT_FALSE(box_clears_disc({0, 0, 80, 80}, 100, 100, 50, 0));
    // Corner exactly at r + margin counts as clear.
    TEST_ASSERT_TRUE(box_clears_disc({0, 0, 70, 60}, 100, 100, 40, 10));
    TEST_ASSERT_FALSE(box_clears_disc({0, 0, 70, 61}, 100, 100, 40, 10));
}

// 480x480 quad-grid tile: the ring leaves no room, the legend would print
// across the dial (the case the old guard was written for).
static void test_legend_dropped_on_square_quad_tile() {
    TEST_ASSERT_FALSE(legend_fits(196, 196));
    TEST_ASSERT_FALSE(legend_fits(236, 236));
}

// Tall freeform tile (288 x 480, a 3:2 row): the legend sits below the ring
// and must be drawn even though the ring's left edge is close to the tile edge.
static void test_legend_kept_on_tall_tile() {
    TEST_ASSERT_TRUE(legend_fits(288, 480));
    TEST_ASSERT_TRUE(legend_fits(240, 480));
}

// Too short for three rows below the caption.
static void test_legend_dropped_on_short_tile() {
    TEST_ASSERT_FALSE(legend_fits(400, 90));
    TEST_ASSERT_TRUE(legend_fits(400, 100));  // rows just fit under the caption
}

// The legend must not collide with the CTS line that tracks the ring bottom.
static void test_legend_avoids_cts_line() {
    // A small ring up and to the right of the rows: the disc (plus marker
    // overhang) is clear, but the CTS line under it reaches the first row.
    const int ch = 200;
    const int gy = compass_legend_top(ch);
    const int dia = 72;
    const int mcx = kLegendInset + kLegendW + kCtsHalfW - 1;
    const int mcy = gy - 55;
    const Box legend = {kLegendInset, gy, kLegendInset + kLegendW, gy + 3 * kLegendRowH};
    TEST_ASSERT_TRUE(box_clears_disc(legend, mcx, mcy, dia / 2, kMarkerOverhang));
    TEST_ASSERT_FALSE(compass_legend_fits(300, ch, mcx, mcy, dia));
}

static void test_band_visible_limits_when_shown() {
    TEST_ASSERT_TRUE(band_numeral_visible(12.0, BandLabel::Shown));
    TEST_ASSERT_FALSE(band_numeral_visible(11.9, BandLabel::Shown));
    TEST_ASSERT_TRUE(band_numeral_visible(-80.0, BandLabel::Shown));
    TEST_ASSERT_FALSE(band_numeral_visible(80.1, BandLabel::Shown));
    TEST_ASSERT_TRUE(band_numeral_visible(45.0, BandLabel::Shown));
}

static void test_band_hidden_needs_hysteresis_to_return() {
    TEST_ASSERT_FALSE(band_numeral_visible(13.0, BandLabel::Hidden));
    TEST_ASSERT_FALSE(band_numeral_visible(-14.9, BandLabel::Hidden));
    TEST_ASSERT_TRUE(band_numeral_visible(15.0, BandLabel::Hidden));
    TEST_ASSERT_FALSE(band_numeral_visible(78.0, BandLabel::Hidden));
    TEST_ASSERT_TRUE(band_numeral_visible(-77.0, BandLabel::Hidden));
}

// First layout (after a build or theme switch): plain limits, no slack, so a
// steady heading shows the same numerals every time the screen is built.
static void test_band_first_layout_uses_plain_limits() {
    TEST_ASSERT_TRUE(band_numeral_visible(13.0, BandLabel::New));
    TEST_ASSERT_TRUE(band_numeral_visible(-77.0, BandLabel::New));
    TEST_ASSERT_TRUE(band_numeral_visible(79.0, BandLabel::New));
    TEST_ASSERT_FALSE(band_numeral_visible(11.0, BandLabel::New));
    TEST_ASSERT_FALSE(band_numeral_visible(81.0, BandLabel::New));
}

// Heading held about 012 deg with +-1 deg of yaw: the "0" numeral (rel = -hdg)
// may change state at most once instead of on every sample.
static void test_band_no_flicker_near_lubber() {
    const double hdgs[] = {12.5, 11.5, 12.4, 11.6, 12.9, 11.2, 12.2, 11.8, 12.6, 11.4};
    bool vis = true;
    int toggles = 0;
    for (double h : hdgs) {
        bool v = band_numeral_visible(-h, vis ? BandLabel::Shown : BandLabel::Hidden);
        if (v != vis) toggles++;
        vis = v;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, toggles);
}

static void check_steer_invariants(int w, int h) {
    SteerSquareLayout L = wind_steer_square_layout(w, h);
    const int course_top = h - kSteerCourseH - kSteerCoursePad;
    TEST_ASSERT_LESS_OR_EQUAL_INT(w - 32, L.compass_w);
    TEST_ASSERT_GREATER_THAN_INT(0, L.compass_w);
    TEST_ASSERT_LESS_OR_EQUAL_INT(course_top, steer_strip_bottom(L.compass_w));
    if (L.tiles) {
        // At least kSteerMinTileH, unless the tiles are narrower (then square).
        const int want = L.tile_w < kSteerMinTileH ? L.tile_w : kSteerMinTileH;
        TEST_ASSERT_GREATER_OR_EQUAL_INT(want, L.tile_h);
        TEST_ASSERT_LESS_OR_EQUAL_INT(L.tile_w, L.tile_h);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(steer_strip_bottom(L.compass_w), L.tile_y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(course_top - kSteerGap, L.tile_y + L.tile_h);
    }
}

// 480x480 is the production panel: geometry must stay exactly as before
// (full-width compass, 84 px tiles right under the XTE strip).
static void test_steer_480_unchanged() {
    SteerSquareLayout L = wind_steer_square_layout(480, 480);
    TEST_ASSERT_EQUAL_INT(448, L.compass_w);
    TEST_ASSERT_TRUE(L.tiles);
    TEST_ASSERT_EQUAL_INT(110, L.tile_w);
    TEST_ASSERT_EQUAL_INT(84, L.tile_h);
    TEST_ASSERT_EQUAL_INT(346, L.tile_y);
}

// 360x360 (Waveshare knob) used to get 24 px tiles under the strip.
static void test_steer_360_keeps_usable_tiles() {
    SteerSquareLayout L = wind_steer_square_layout(360, 360);
    TEST_ASSERT_TRUE(L.tiles);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(kSteerMinTileH, L.tile_h);
    check_steer_invariants(360, 360);
}

// 240x240 (Waveshare 1.28) used to get a negative tile height and an XTE strip
// running under the course row.
static void test_steer_240_never_negative() {
    SteerSquareLayout L = wind_steer_square_layout(240, 240);
    TEST_ASSERT_FALSE(L.tiles);
    // Largest compass whose XTE strip ends at the course row (y 198).
    TEST_ASSERT_EQUAL_INT(152, L.compass_w);
    TEST_ASSERT_EQUAL_INT(198, steer_strip_bottom(L.compass_w));
    check_steer_invariants(240, 240);
}

static void test_steer_sweep() {
    for (int s = 200; s <= 720; s += 8)
        check_steer_invariants(s, s);
    // Portrait panels too (the non-wide branch covers them).
    for (int h = 360; h <= 800; h += 40)
        check_steer_invariants(320, h);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_box_clears_disc);
    RUN_TEST(test_legend_dropped_on_square_quad_tile);
    RUN_TEST(test_legend_kept_on_tall_tile);
    RUN_TEST(test_legend_dropped_on_short_tile);
    RUN_TEST(test_legend_avoids_cts_line);
    RUN_TEST(test_band_visible_limits_when_shown);
    RUN_TEST(test_band_hidden_needs_hysteresis_to_return);
    RUN_TEST(test_band_first_layout_uses_plain_limits);
    RUN_TEST(test_band_no_flicker_near_lubber);
    RUN_TEST(test_steer_480_unchanged);
    RUN_TEST(test_steer_360_keeps_usable_tiles);
    RUN_TEST(test_steer_240_never_negative);
    RUN_TEST(test_steer_sweep);
    return UNITY_END();
}
