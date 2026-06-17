#include <unity.h>
#include <cstring>
#include "layout.h"

using namespace layout;

void setUp(void) {
}
void tearDown(void) {
}

// ---- pure decision function: zoom_action ---------------------------------

static void test_not_zoomable_is_none() {
    // Explicit zoomable=false → no zoom regardless of target.
    TEST_ASSERT_EQUAL(ZOOM_NONE, zoom_action(false, "auto"));
    TEST_ASSERT_EQUAL(ZOOM_NONE, zoom_action(false, "wind"));
    TEST_ASSERT_EQUAL(ZOOM_NONE, zoom_action(false, ""));
}

static void test_auto_scales_in_place() {
    TEST_ASSERT_EQUAL(ZOOM_AUTO_SCALE, zoom_action(true, "auto"));
}

static void test_empty_target_defaults_to_auto() {
    // A zoomable field with no explicit target still scales in place.
    TEST_ASSERT_EQUAL(ZOOM_AUTO_SCALE, zoom_action(true, ""));
    TEST_ASSERT_EQUAL(ZOOM_AUTO_SCALE, zoom_action(true, nullptr));
}

static void test_screen_id_target_shows_screen() {
    TEST_ASSERT_EQUAL(ZOOM_SHOW_SCREEN, zoom_action(true, "wind"));
    TEST_ASSERT_EQUAL(ZOOM_SHOW_SCREEN, zoom_action(true, "dashboard"));
}

// ---- default_zoomable: numeric defaults true, others false ---------------

static void test_default_zoomable_numeric_true() {
    TEST_ASSERT_TRUE(default_zoomable("numeric"));
    // Empty / unknown widget falls back to the numeric default in the
    // renderer, so treat it as zoomable too.
    TEST_ASSERT_TRUE(default_zoomable(""));
    TEST_ASSERT_TRUE(default_zoomable(nullptr));
}

static void test_default_zoomable_nonnumeric_false() {
    TEST_ASSERT_FALSE(default_zoomable("compass"));
    TEST_ASSERT_FALSE(default_zoomable("windRose"));
    TEST_ASSERT_FALSE(default_zoomable("gauge"));
    TEST_ASSERT_FALSE(default_zoomable("text"));
    TEST_ASSERT_FALSE(default_zoomable("button"));
    TEST_ASSERT_FALSE(default_zoomable("autopilot"));
}

// ---- parse() carries zoomable + zoom onto the tile -----------------------

static void test_parse_zoom_explicit() {
    const char *j = "{\"screens\":[{\"id\":\"d\",\"type\":\"quadrants\",\"tiles\":["
                    "  {\"id\":\"a\",\"widget\":\"compass\",\"zoomable\":true,\"zoom\":\"wind\"}"
                    "]}]}";
    Config c;
    TEST_ASSERT_EQUAL(0, parse(j, strlen(j), c));
    const Tile &t = c.screens[0].tiles[0];
    TEST_ASSERT_TRUE(t.zoomable);
    TEST_ASSERT_EQUAL_STRING("wind", t.zoom);
    // Decision: a zoomable compass with a screen-id target opens that screen.
    TEST_ASSERT_EQUAL(ZOOM_SHOW_SCREEN, zoom_action(t.zoomable, t.zoom));
}

static void test_parse_zoom_numeric_default_true() {
    // A numeric tile with neither zoomable nor zoom → defaults to zoomable
    // auto-scale.
    const char *j =
        "{\"screens\":[{\"id\":\"d\",\"type\":\"quadrants\",\"tiles\":["
        "  {\"id\":\"a\",\"widget\":\"numeric\",\"primary\":\"environment.depth.belowTransducer\"}"
        "]}]}";
    Config c;
    TEST_ASSERT_EQUAL(0, parse(j, strlen(j), c));
    const Tile &t = c.screens[0].tiles[0];
    TEST_ASSERT_TRUE(t.zoomable);
    TEST_ASSERT_EQUAL_STRING("auto", t.zoom);
    TEST_ASSERT_EQUAL(ZOOM_AUTO_SCALE, zoom_action(t.zoomable, t.zoom));
}

static void test_parse_zoom_nonnumeric_default_false() {
    // A compass tile with no zoom keys → not zoomable by default.
    const char *j = "{\"screens\":[{\"id\":\"d\",\"type\":\"quadrants\",\"tiles\":["
                    "  {\"id\":\"a\",\"widget\":\"compass\"}"
                    "]}]}";
    Config c;
    TEST_ASSERT_EQUAL(0, parse(j, strlen(j), c));
    const Tile &t = c.screens[0].tiles[0];
    TEST_ASSERT_FALSE(t.zoomable);
    TEST_ASSERT_EQUAL(ZOOM_NONE, zoom_action(t.zoomable, t.zoom));
}

static void test_parse_zoomable_false_overrides() {
    // Numeric, but explicitly zoomable:false → None.
    const char *j = "{\"screens\":[{\"id\":\"d\",\"type\":\"quadrants\",\"tiles\":["
                    "  {\"id\":\"a\",\"widget\":\"numeric\",\"zoomable\":false}"
                    "]}]}";
    Config c;
    TEST_ASSERT_EQUAL(0, parse(j, strlen(j), c));
    const Tile &t = c.screens[0].tiles[0];
    TEST_ASSERT_FALSE(t.zoomable);
    TEST_ASSERT_EQUAL(ZOOM_NONE, zoom_action(t.zoomable, t.zoom));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_not_zoomable_is_none);
    RUN_TEST(test_auto_scales_in_place);
    RUN_TEST(test_empty_target_defaults_to_auto);
    RUN_TEST(test_screen_id_target_shows_screen);
    RUN_TEST(test_default_zoomable_numeric_true);
    RUN_TEST(test_default_zoomable_nonnumeric_false);
    RUN_TEST(test_parse_zoom_explicit);
    RUN_TEST(test_parse_zoom_numeric_default_true);
    RUN_TEST(test_parse_zoom_nonnumeric_default_false);
    RUN_TEST(test_parse_zoomable_false_overrides);
    return UNITY_END();
}
