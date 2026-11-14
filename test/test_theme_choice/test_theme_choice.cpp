// Host tests for include/theme_choice.h (instruments#66): the Settings THEME
// row must select the #61 skins and highlight the right button for every
// persisted theme value, including the pre-#61 "day" / "night".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <unity.h>

#include "theme_choice.h"
#include "ui_config_check.h"

using namespace theme_choice;

void setUp(void) {
}
void tearDown(void) {
}

static void test_buttons_select_sun_red_night_auto() {
    TEST_ASSERT_EQUAL_STRING("sun", theme_for(kDay));
    TEST_ASSERT_EQUAL_STRING("red-night", theme_for(kNight));
    TEST_ASSERT_EQUAL_STRING("auto", theme_for(kAuto));
}

static void test_out_of_range_index_is_null() {
    TEST_ASSERT_NULL(theme_for(-1));
    TEST_ASSERT_NULL(theme_for(kCount));
}

static void test_every_choice_is_an_accepted_theme_token() {
    for (int i = 0; i < kCount; ++i)
        TEST_ASSERT_TRUE(ui_config::is_valid_theme(theme_for(i)));
}

static void test_choice_round_trips_to_its_button() {
    for (int i = 0; i < kCount; ++i)
        TEST_ASSERT_EQUAL_INT(i, index_for(theme_for(i)));
}

static void test_legacy_day_night_highlight_day_night() {
    TEST_ASSERT_EQUAL_INT(kDay, index_for("day"));
    TEST_ASSERT_EQUAL_INT(kNight, index_for("night"));
}

static void test_sun_highlights_day_not_night() {
    TEST_ASSERT_EQUAL_INT(kDay, index_for("sun"));
}

static void test_other_palettes_highlight_nothing() {
    TEST_ASSERT_EQUAL_INT(kNone, index_for("high-contrast"));
    TEST_ASSERT_EQUAL_INT(kNone, index_for("classic"));
    TEST_ASSERT_EQUAL_INT(kNone, index_for("bogus"));
    TEST_ASSERT_EQUAL_INT(kNone, index_for(""));
    TEST_ASSERT_EQUAL_INT(kNone, index_for(nullptr));
}

static void test_match_is_case_sensitive() {
    TEST_ASSERT_EQUAL_INT(kNone, index_for("Sun"));
    TEST_ASSERT_EQUAL_INT(kNone, index_for("NIGHT"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_buttons_select_sun_red_night_auto);
    RUN_TEST(test_out_of_range_index_is_null);
    RUN_TEST(test_every_choice_is_an_accepted_theme_token);
    RUN_TEST(test_choice_round_trips_to_its_button);
    RUN_TEST(test_legacy_day_night_highlight_day_night);
    RUN_TEST(test_sun_highlights_day_not_night);
    RUN_TEST(test_other_palettes_highlight_nothing);
    RUN_TEST(test_match_is_case_sensitive);
    return UNITY_END();
}
