// Host tests for the ui.* validators consumed by manager apply_config
// and the brightness.set v1 command. Pinning the rules here means a
// refactor can't accidentally widen what's accepted.
//
// Also hosts the config_model theme-mapping round-trip tests. The native
// build_src_filter does not compile src/config_model.cpp, so it is compiled
// into this test TU directly (it is host-clean: string.h + math.h only).

#include <unity.h>

#include "ui_config_check.h"

#include "../../src/config_model.cpp"  // theme_name / parse_theme / clamp_ui

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
    // Catalog theme implemented in firmware + the two firmware-extra skins.
    TEST_ASSERT_TRUE(is_valid_theme("high-contrast"));
    TEST_ASSERT_TRUE(is_valid_theme("red-night"));
    TEST_ASSERT_TRUE(is_valid_theme("classic"));
}

static void test_theme_unknown_rejected() {
    TEST_ASSERT_FALSE(is_valid_theme(""));
    TEST_ASSERT_FALSE(is_valid_theme("dusk"));
    TEST_ASSERT_FALSE(is_valid_theme("DAY"));  // case-sensitive
    TEST_ASSERT_FALSE(is_valid_theme("Night"));
    TEST_ASSERT_FALSE(is_valid_theme("auto "));
    TEST_ASSERT_FALSE(is_valid_theme("high_contrast"));  // hyphen, not underscore
    TEST_ASSERT_FALSE(is_valid_theme("rednight"));
}

static void test_theme_null_rejected() {
    TEST_ASSERT_FALSE(is_valid_theme(nullptr));
}

// ---- config_model theme mapping -------------------------------------------

static void test_theme_name_parse_round_trip() {
    // Every enum value must survive name -> parse -> name unchanged.
    const config::Theme all[] = {config::Theme::Night, config::Theme::Day,
                                 config::Theme::HighContrast, config::Theme::RedNight,
                                 config::Theme::Classic};
    for (config::Theme t : all) {
        const char *n = config::theme_name(t);
        config::Theme back = config::parse_theme(n, config::Theme::Night);
        TEST_ASSERT_EQUAL_MESSAGE((int)t, (int)back, n);
    }
}

static void test_theme_parse_canonical_names() {
    TEST_ASSERT_EQUAL((int)config::Theme::Day,
                      (int)config::parse_theme("day", config::Theme::Night));
    TEST_ASSERT_EQUAL((int)config::Theme::Night,
                      (int)config::parse_theme("night", config::Theme::Day));
    TEST_ASSERT_EQUAL((int)config::Theme::HighContrast,
                      (int)config::parse_theme("high-contrast", config::Theme::Night));
    TEST_ASSERT_EQUAL((int)config::Theme::RedNight,
                      (int)config::parse_theme("red-night", config::Theme::Night));
    TEST_ASSERT_EQUAL((int)config::Theme::Classic,
                      (int)config::parse_theme("classic", config::Theme::Night));
}

static void test_theme_parse_unknown_falls_back() {
    TEST_ASSERT_EQUAL((int)config::Theme::Day,
                      (int)config::parse_theme("dusk", config::Theme::Day));
    TEST_ASSERT_EQUAL((int)config::Theme::Night,
                      (int)config::parse_theme(nullptr, config::Theme::Night));
    // Legacy "auto" is not a palette; it falls back too.
    TEST_ASSERT_EQUAL((int)config::Theme::Night,
                      (int)config::parse_theme("auto", config::Theme::Night));
}

static void test_clamp_ui_accepts_all_themes_rejects_beyond() {
    config::UiConfig c;
    // Every named theme survives clamping untouched.
    const config::Theme all[] = {config::Theme::Night, config::Theme::Day,
                                 config::Theme::HighContrast, config::Theme::RedNight,
                                 config::Theme::Classic};
    for (config::Theme t : all) {
        c.theme = t;
        TEST_ASSERT_TRUE_MESSAGE(config::clamp_ui(c), config::theme_name(t));
        TEST_ASSERT_EQUAL((int)t, (int)c.theme);
    }
    // A stale/corrupt NVS value past the last enum resets to Night.
    c.theme = (config::Theme)((uint8_t)config::Theme::Classic + 1);
    TEST_ASSERT_FALSE(config::clamp_ui(c));
    TEST_ASSERT_EQUAL((int)config::Theme::Night, (int)c.theme);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_brightness_extremes_accepted);
    RUN_TEST(test_brightness_midrange_accepted);
    RUN_TEST(test_brightness_out_of_range_rejected);
    RUN_TEST(test_theme_supported_tokens_accepted);
    RUN_TEST(test_theme_unknown_rejected);
    RUN_TEST(test_theme_null_rejected);
    RUN_TEST(test_theme_name_parse_round_trip);
    RUN_TEST(test_theme_parse_canonical_names);
    RUN_TEST(test_theme_parse_unknown_falls_back);
    RUN_TEST(test_clamp_ui_accepts_all_themes_rejects_beyond);
    return UNITY_END();
}
