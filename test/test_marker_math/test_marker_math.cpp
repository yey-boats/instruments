#include <math.h>
#include <string.h>
#include <unity.h>

#include "marker_math.h"

using ui::GlyphId;

void setUp() {
}
void tearDown() {
}

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
    TEST_ASSERT_FALSE(ui::marker_occluded(96.0, 96.0));   // exactly at edge -> visible
    TEST_ASSERT_FALSE(ui::marker_occluded(264.0, 96.0));  // folds to -96 -> visible
}

static void test_occluded_bottom_hidden() {
    TEST_ASSERT_TRUE(ui::marker_occluded(120.0, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(180.0, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(NAN, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(97.0, 96.0));
    TEST_ASSERT_TRUE(ui::marker_occluded(263.0, 96.0));  // folds to -97 -> hidden
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
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlyphId::COUNT, (uint8_t)ui::glyph_from_token(nullptr));
}

// ---- close-angle radial stagger ---------------------------------------------

static void test_stagger_separated_all_level0() {
    const double a[4] = {0.0, 45.0, 180.0, 270.0};
    uint8_t lv[4] = {9, 9, 9, 9};
    ui::marker_stagger_levels(a, 4, 8.0, lv);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, lv[i]);
}

static void test_stagger_pair_within_threshold() {
    const double a[2] = {100.0, 104.0};  // 4 deg apart -> second steps inward
    uint8_t lv[2];
    ui::marker_stagger_levels(a, 2, 8.0, lv);
    TEST_ASSERT_EQUAL_UINT8(0, lv[0]);
    TEST_ASSERT_EQUAL_UINT8(1, lv[1]);
}

static void test_stagger_triple_coincident_stacks() {
    const double a[3] = {56.0, 56.0, 57.0};
    uint8_t lv[3];
    ui::marker_stagger_levels(a, 3, 8.0, lv);
    TEST_ASSERT_EQUAL_UINT8(0, lv[0]);
    TEST_ASSERT_EQUAL_UINT8(1, lv[1]);
    TEST_ASSERT_EQUAL_UINT8(2, lv[2]);
}

static void test_stagger_wraps_around_north() {
    const double a[2] = {358.0, 2.0};  // 4 deg apart across 0
    uint8_t lv[2];
    ui::marker_stagger_levels(a, 2, 8.0, lv);
    TEST_ASSERT_EQUAL_UINT8(0, lv[0]);
    TEST_ASSERT_EQUAL_UINT8(1, lv[1]);
}

static void test_stagger_hidden_nan_never_pushes() {
    const double a[3] = {90.0, NAN, 92.0};  // NaN (hidden) between two visibles
    uint8_t lv[3];
    ui::marker_stagger_levels(a, 3, 8.0, lv);
    TEST_ASSERT_EQUAL_UINT8(0, lv[0]);
    TEST_ASSERT_EQUAL_UINT8(0, lv[1]);  // hidden -> level 0
    TEST_ASSERT_EQUAL_UINT8(1, lv[2]);  // staggered against the FIRST, not the NaN
}

static void test_stagger_level_caps_at_3() {
    const double a[6] = {10.0, 10.0, 10.0, 10.0, 10.0, 10.0};
    uint8_t lv[6];
    ui::marker_stagger_levels(a, 6, 8.0, lv);
    TEST_ASSERT_EQUAL_UINT8(3, lv[4]);
    TEST_ASSERT_EQUAL_UINT8(3, lv[5]);
}

// ---- XTE needle sign convention ----------------------------------------------
// Text convention everywhere in the firmware: positive xte -> 'P' suffix
// (ui::format_xte + the QuadGrid XTE tile). The needle must deflect toward the
// side the letter names, i.e. positive xte -> PORT -> negative fraction.

static void test_xte_needle_positive_deflects_port() {
    // 0.15 nm right-of-track reads "0.15 nm P" -> needle on the PORT (left) end.
    double f = ui::xte_needle_frac(0.15 * 1852.0, 1852.0);
    TEST_ASSERT_TRUE(f < 0.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -0.15, f);
}

static void test_xte_needle_negative_deflects_starboard() {
    double f = ui::xte_needle_frac(-0.5 * 1852.0, 1852.0);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, f);
}

static void test_xte_needle_clamps_to_full_scale() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -1.0, ui::xte_needle_frac(10 * 1852.0, 1852.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, ui::xte_needle_frac(-10 * 1852.0, 1852.0));
}

static void test_xte_needle_nan_centers() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, ui::xte_needle_frac(NAN, 1852.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, ui::xte_needle_frac(100.0, 0.0));  // bad scale
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
    RUN_TEST(test_stagger_separated_all_level0);
    RUN_TEST(test_stagger_pair_within_threshold);
    RUN_TEST(test_stagger_triple_coincident_stacks);
    RUN_TEST(test_stagger_wraps_around_north);
    RUN_TEST(test_stagger_hidden_nan_never_pushes);
    RUN_TEST(test_stagger_level_caps_at_3);
    RUN_TEST(test_xte_needle_positive_deflects_port);
    RUN_TEST(test_xte_needle_negative_deflects_starboard);
    RUN_TEST(test_xte_needle_clamps_to_full_scale);
    RUN_TEST(test_xte_needle_nan_centers);
    return UNITY_END();
}
