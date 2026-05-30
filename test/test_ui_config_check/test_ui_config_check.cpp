// Host tests for the ui.* validators consumed by manager apply_config
// and the brightness.set v1 command. Pinning the rules here means a
// refactor can't accidentally widen what's accepted.

#include <unity.h>

#include "ui_config_check.h"

using ui_config::is_valid_brightness;
using ui_config::is_valid_theme;

void setUp(void) {
}
void tearDown(void) {
}

// ---- brightness ----------------------------------------------------------

static void test_brightness_extremes_accepted() {
    TEST_ASSERT_TRUE(is_valid_brightness(0));
    TEST_ASSERT_TRUE(is_valid_brightness(255));
}

static void test_brightness_midrange_accepted() {
    TEST_ASSERT_TRUE(is_valid_brightness(1));
    TEST_ASSERT_TRUE(is_valid_brightness(128));
    TEST_ASSERT_TRUE(is_valid_brightness(254));
}

static void test_brightness_out_of_range_rejected() {
    TEST_ASSERT_FALSE(is_valid_brightness(-1));
    TEST_ASSERT_FALSE(is_valid_brightness(256));
    TEST_ASSERT_FALSE(is_valid_brightness(1000));
    TEST_ASSERT_FALSE(is_valid_brightness(-1000));
}

// ---- theme ---------------------------------------------------------------

static void test_theme_supported_tokens_accepted() {
    TEST_ASSERT_TRUE(is_valid_theme("day"));
    TEST_ASSERT_TRUE(is_valid_theme("night"));
    TEST_ASSERT_TRUE(is_valid_theme("auto"));
}

static void test_theme_unknown_rejected() {
    TEST_ASSERT_FALSE(is_valid_theme(""));
    TEST_ASSERT_FALSE(is_valid_theme("dusk"));
    TEST_ASSERT_FALSE(is_valid_theme("DAY"));  // case-sensitive
    TEST_ASSERT_FALSE(is_valid_theme("Night"));
    TEST_ASSERT_FALSE(is_valid_theme("auto "));
}

static void test_theme_null_rejected() {
    TEST_ASSERT_FALSE(is_valid_theme(nullptr));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_brightness_extremes_accepted);
    RUN_TEST(test_brightness_midrange_accepted);
    RUN_TEST(test_brightness_out_of_range_rejected);
    RUN_TEST(test_theme_supported_tokens_accepted);
    RUN_TEST(test_theme_unknown_rejected);
    RUN_TEST(test_theme_null_rejected);
    return UNITY_END();
}
