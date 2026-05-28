// Host tests for board:: + ui::LayoutContext. Spec 13 step 9.
//
// These run against the BOARD_ID_NATIVE_FAKE board, so we get
// deterministic geometry + capabilities + layout-context math without
// pulling in Arduino. The fake's geometry can be overridden at compile
// time with -DFAKE_BOARD_WIDTH / -DFAKE_BOARD_HEIGHT, but the default
// 480x480 mirrors the production board.

#include <unity.h>
#include <string.h>

#include "board.h"

void setUp(void) {}
void tearDown(void) {}

static void test_id_and_display_name_set() {
    TEST_ASSERT_NOT_NULL(board::id());
    TEST_ASSERT_TRUE(strlen(board::id()) > 0);
    TEST_ASSERT_EQUAL_STRING("native_fake", board::id());
    TEST_ASSERT_NOT_NULL(board::display_name());
    TEST_ASSERT_TRUE(strlen(board::display_name()) > 0);
}

static void test_geometry_defaults_480_square() {
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_UINT16(480, g.width_px);
    TEST_ASSERT_EQUAL_UINT16(480, g.height_px);
    TEST_ASSERT_TRUE(g.square);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::SquareCompact),
                          static_cast<int>(g.layout_class));
}

static void test_capabilities_default_off() {
    auto c = board::capabilities();
    // The fake board has no real peripherals - everything should be
    // disabled by default so tests for "feature available?" branches
    // exercise the negative path.
    TEST_ASSERT_FALSE(c.psram_required);
    TEST_ASSERT_FALSE(c.touch_calibration);
    TEST_ASSERT_FALSE(c.beeper);
    TEST_ASSERT_FALSE(c.nmea2000_can);
    TEST_ASSERT_FALSE(c.sd_card);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::TouchKind::None),
                          static_cast<int>(c.touch));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::BacklightKind::None),
                          static_cast<int>(c.backlight));
}

static void test_backlight_setter_roundtrips() {
    board::set_backlight(0);
    TEST_ASSERT_EQUAL_UINT8(0, board::backlight());
    board::set_backlight(128);
    TEST_ASSERT_EQUAL_UINT8(128, board::backlight());
    board::set_backlight(255);
    TEST_ASSERT_EQUAL_UINT8(255, board::backlight());
}

static void test_set_power_does_not_crash() {
    TEST_ASSERT_TRUE(board::set_power(false));
    TEST_ASSERT_TRUE(board::set_power(true));
}

static void test_layout_context_square_matches_geometry() {
    auto ctx = ui::layout_context();
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_UINT16(g.width_px, ctx.w);
    TEST_ASSERT_EQUAL_UINT16(g.height_px, ctx.h);
    TEST_ASSERT_EQUAL_UINT16(g.width_px, ctx.short_side);
    TEST_ASSERT_EQUAL_UINT16(g.height_px, ctx.long_side);
    TEST_ASSERT_TRUE(ctx.square);
    TEST_ASSERT_FALSE(ctx.landscape);
}

static void test_layout_context_touch_targets_44_on_small() {
    // 480-class panel should use the 44 px touch target per spec 13
    // §"Screen Safe Area".
    auto ctx = ui::layout_context();
    TEST_ASSERT_EQUAL_UINT16(44, ctx.touch_min);
    TEST_ASSERT_EQUAL_UINT16(8, ctx.margin);
    TEST_ASSERT_EQUAL_UINT16(4, ctx.gap);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_id_and_display_name_set);
    RUN_TEST(test_geometry_defaults_480_square);
    RUN_TEST(test_capabilities_default_off);
    RUN_TEST(test_backlight_setter_roundtrips);
    RUN_TEST(test_set_power_does_not_crash);
    RUN_TEST(test_layout_context_square_matches_geometry);
    RUN_TEST(test_layout_context_touch_targets_44_on_small);
    return UNITY_END();
}
