#include <math.h>
#include <string.h>

#include <unity.h>

#include "value_format.h"

using vfmt::format_scaled;
using vfmt::UnitFormat;

static const char *fmt(double v, uint8_t decimals, bool si) {
    static char buf[24];
    UnitFormat f;
    f.decimals = decimals;
    f.si_prefix = si;
    return format_scaled(v, f, buf, sizeof(buf));
}

static void test_plain_decimals_no_prefix() {
    TEST_ASSERT_EQUAL_STRING("2.1", fmt(2.063, 1, false));
    TEST_ASSERT_EQUAL_STRING("-1.8", fmt(-1.83, 1, false));
    TEST_ASSERT_EQUAL_STRING("12", fmt(12.3, 0, false));
}

static void test_si_prefix_kilo() {
    // 1234.5 nm -> 1.23k ; 2841 m -> 2.8k
    TEST_ASSERT_EQUAL_STRING("1.23k", fmt(1234.5, 2, true));
    TEST_ASSERT_EQUAL_STRING("2.8k", fmt(2841.0, 1, true));
}

static void test_si_prefix_mega_giga() {
    TEST_ASSERT_EQUAL_STRING("1.50M", fmt(1500000.0, 2, true));
    TEST_ASSERT_EQUAL_STRING("2.0G", fmt(2.0e9, 1, true));
}

static void test_below_threshold_stays_plain() {
    // Under 1000 keeps normal decimals even with si_prefix on.
    TEST_ASSERT_EQUAL_STRING("5.3", fmt(5.3, 1, true));
    TEST_ASSERT_EQUAL_STRING("999.9", fmt(999.9, 1, true));
}

static void test_negative_with_prefix() {
    TEST_ASSERT_EQUAL_STRING("-1.23k", fmt(-1234.5, 2, true));
}

static void test_nan_is_placeholder() {
    TEST_ASSERT_EQUAL_STRING("--", fmt(NAN, 1, true));
    TEST_ASSERT_EQUAL_STRING("--", fmt(NAN, 0, false));
}

static void test_exact_thousand_scales() {
    TEST_ASSERT_EQUAL_STRING("1.0k", fmt(1000.0, 1, true));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_decimals_no_prefix);
    RUN_TEST(test_si_prefix_kilo);
    RUN_TEST(test_si_prefix_mega_giga);
    RUN_TEST(test_below_threshold_stays_plain);
    RUN_TEST(test_negative_with_prefix);
    RUN_TEST(test_nan_is_placeholder);
    RUN_TEST(test_exact_thousand_scales);
    return UNITY_END();
}
