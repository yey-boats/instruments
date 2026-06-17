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
