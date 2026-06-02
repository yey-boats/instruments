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
#include "boards/board_native_fake.h"

void setUp(void) {
}
void tearDown(void) {
    board::native_fake::reset_geometry();
}

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
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DisplayShape::Square), static_cast<int>(g.shape));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::SquareCompact),
                          static_cast<int>(g.layout_class));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DensityClass::Mdpi),
                          static_cast<int>(g.density_class));
    TEST_ASSERT_EQUAL_UINT16(0, g.usable_x);
    TEST_ASSERT_EQUAL_UINT16(0, g.usable_y);
    TEST_ASSERT_EQUAL_UINT16(480, g.usable_width);
    TEST_ASSERT_EQUAL_UINT16(480, g.usable_height);
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
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::TouchKind::None), static_cast<int>(c.touch));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DisplayBus::RgbParallel),
                          static_cast<int>(c.display_bus));
    TEST_ASSERT_FALSE(c.touch_interrupt);
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

// ---- Multi-shape coverage via the native_fake override -------------------
//
// A single test binary, three boards. set_geometry() lets us exercise
// the layout-class classifier and the touch-target / margin / gap
// heuristics on shapes we don't have hardware for yet.

static void test_800x480_large_panel_uses_compact_class_but_wide_context() {
    board::native_fake::set_geometry(800, 480, 70);
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_UINT16(800, g.width_px);
    TEST_ASSERT_EQUAL_UINT16(480, g.height_px);
    TEST_ASSERT_FALSE(g.square);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DisplayShape::Rectangle),
                          static_cast<int>(g.shape));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::LandscapeCompact),
                          static_cast<int>(g.layout_class));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DensityClass::Mdpi),
                          static_cast<int>(g.density_class));

    auto ctx = ui::layout_context();
    TEST_ASSERT_FALSE(ctx.square);
    TEST_ASSERT_TRUE(ctx.landscape);
    TEST_ASSERT_TRUE(ctx.wide);
    TEST_ASSERT_EQUAL_UINT16(480, ctx.short_side);
    TEST_ASSERT_EQUAL_UINT16(800, ctx.long_side);
    // 480-class short side still uses 44 px touch, 8 px margin.
    TEST_ASSERT_EQUAL_UINT16(44, ctx.touch_min);
    TEST_ASSERT_EQUAL_UINT16(8, ctx.margin);
}

static void test_landscape_1024x600_uses_hdpi_spacing() {
    board::native_fake::set_geometry(1024, 600, 70);
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::LandscapeWide),
                          static_cast<int>(g.layout_class));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DensityClass::Hdpi),
                          static_cast<int>(g.density_class));
    auto ctx = ui::layout_context();
    TEST_ASSERT_EQUAL_UINT16(56, ctx.touch_min);
    TEST_ASSERT_EQUAL_UINT16(16, ctx.margin);
    TEST_ASSERT_EQUAL_UINT16(8, ctx.gap);
}

static void test_round_480_safe_area_inset() {
    board::native_fake::set_geometry(480, 480, 21, board::DisplayShape::Round);
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::DisplayShape::Round), static_cast<int>(g.shape));
    TEST_ASSERT_EQUAL_UINT16(48, g.usable_x);
    TEST_ASSERT_EQUAL_UINT16(48, g.usable_y);
    TEST_ASSERT_EQUAL_UINT16(384, g.usable_width);
    TEST_ASSERT_EQUAL_UINT16(384, g.usable_height);
    auto ctx = ui::layout_context();
    TEST_ASSERT_EQUAL_UINT16(384, ctx.short_side);
}

static void test_board_name_helpers() {
    TEST_ASSERT_EQUAL_STRING("square", board::shape_name(board::DisplayShape::Square));
    TEST_ASSERT_EQUAL_STRING("round", board::shape_name(board::DisplayShape::Round));
    TEST_ASSERT_EQUAL_STRING("hdpi", board::density_class_name(board::DensityClass::Hdpi));
    TEST_ASSERT_EQUAL_STRING("landscape-1024x600",
                             board::layout_class_name(board::LayoutClass::LandscapeWide));
    TEST_ASSERT_EQUAL_STRING("GT911", board::touch_kind_name(board::TouchKind::GT911));
    TEST_ASSERT_EQUAL_STRING("rgb-parallel",
                             board::display_bus_name(board::DisplayBus::RgbParallel));
}

static void test_landscape_compact_640x480() {
    // 640/480 = 1.33 -> not wide; should be LandscapeCompact.
    board::native_fake::set_geometry(640, 480, 50);
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::LandscapeCompact),
                          static_cast<int>(g.layout_class));
    auto ctx = ui::layout_context();
    TEST_ASSERT_TRUE(ctx.landscape);
    // 5.0" with 640 width is below the wide threshold (diag<70 AND
    // width<800), so wide must be false.
    TEST_ASSERT_FALSE(ctx.wide);
}

static void test_portrait_compact_480x640() {
    board::native_fake::set_geometry(480, 640, 50);
    auto g = board::geometry();
    TEST_ASSERT_FALSE(g.square);
    // 640/480 = 1.33 -> Compact, not Tall
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::PortraitCompact),
                          static_cast<int>(g.layout_class));
    auto ctx = ui::layout_context();
    TEST_ASSERT_FALSE(ctx.square);
    TEST_ASSERT_FALSE(ctx.landscape);
    TEST_ASSERT_EQUAL_UINT16(480, ctx.short_side);
    TEST_ASSERT_EQUAL_UINT16(640, ctx.long_side);
}

static void test_portrait_tall_320x800() {
    board::native_fake::set_geometry(320, 800, 35);
    auto g = board::geometry();
    // 800/320 = 2.5 >= 1.5 -> PortraitTall
    TEST_ASSERT_EQUAL_INT(static_cast<int>(board::LayoutClass::PortraitTall),
                          static_cast<int>(g.layout_class));
    auto ctx = ui::layout_context();
    TEST_ASSERT_FALSE(ctx.landscape);
    TEST_ASSERT_FALSE(ctx.wide);
    TEST_ASSERT_EQUAL_UINT16(320, ctx.short_side);
}

static void test_reset_returns_to_compile_time_defaults() {
    board::native_fake::set_geometry(800, 480, 70);
    TEST_ASSERT_EQUAL_UINT16(800, board::geometry().width_px);
    board::native_fake::reset_geometry();
    TEST_ASSERT_EQUAL_UINT16(480, board::geometry().width_px);
    TEST_ASSERT_EQUAL_UINT16(480, board::geometry().height_px);
}

static void test_set_geometry_zero_keeps_current() {
    board::native_fake::set_geometry(800, 480, 70);
    board::native_fake::set_geometry(0, 0, 90);  // only diag changes
    auto g = board::geometry();
    TEST_ASSERT_EQUAL_UINT16(800, g.width_px);
    TEST_ASSERT_EQUAL_UINT16(480, g.height_px);
    TEST_ASSERT_EQUAL_UINT16(90, g.diagonal_tenths_in);
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
    RUN_TEST(test_800x480_large_panel_uses_compact_class_but_wide_context);
    RUN_TEST(test_landscape_1024x600_uses_hdpi_spacing);
    RUN_TEST(test_round_480_safe_area_inset);
    RUN_TEST(test_board_name_helpers);
    RUN_TEST(test_landscape_compact_640x480);
    RUN_TEST(test_portrait_compact_480x640);
    RUN_TEST(test_portrait_tall_320x800);
    RUN_TEST(test_reset_returns_to_compile_time_defaults);
    RUN_TEST(test_set_geometry_zero_keeps_current);
    return UNITY_END();
}
