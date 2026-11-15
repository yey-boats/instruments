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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_not_zoomable_is_none);
    RUN_TEST(test_auto_scales_in_place);
    RUN_TEST(test_empty_target_defaults_to_auto);
    RUN_TEST(test_screen_id_target_shows_screen);
    return UNITY_END();
}
