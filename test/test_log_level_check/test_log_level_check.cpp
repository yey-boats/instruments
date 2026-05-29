// Host tests for the log-level parser shared by spec 17 §8 log.level
// and spec 17 §6 cfg["debug"]["logLevel"]. The values returned must
// match ESP_LOG_* (0..5) so the firmware can cast straight to
// esp_log_level_t.

#include <unity.h>

#include "log_level_check.h"

using log_level_check::from_string;
using log_level_check::is_valid_int;

void setUp(void) {}
void tearDown(void) {}

static void test_string_known_tokens_accepted() {
    int out = -1;
    TEST_ASSERT_TRUE(from_string("none", &out));    TEST_ASSERT_EQUAL_INT(0, out);
    TEST_ASSERT_TRUE(from_string("error", &out));   TEST_ASSERT_EQUAL_INT(1, out);
    TEST_ASSERT_TRUE(from_string("warn", &out));    TEST_ASSERT_EQUAL_INT(2, out);
    TEST_ASSERT_TRUE(from_string("info", &out));    TEST_ASSERT_EQUAL_INT(3, out);
    TEST_ASSERT_TRUE(from_string("debug", &out));   TEST_ASSERT_EQUAL_INT(4, out);
    TEST_ASSERT_TRUE(from_string("trace", &out));   TEST_ASSERT_EQUAL_INT(5, out);
    TEST_ASSERT_TRUE(from_string("verbose", &out)); TEST_ASSERT_EQUAL_INT(5, out);
}

static void test_string_unknown_rejected() {
    int out = 99;
    TEST_ASSERT_FALSE(from_string("spammy", &out));
    TEST_ASSERT_FALSE(from_string("INFO", &out));   // case-sensitive
    TEST_ASSERT_FALSE(from_string("Debug", &out));
    TEST_ASSERT_FALSE(from_string("info ", &out));  // trailing space
    TEST_ASSERT_EQUAL_INT(99, out);  // unchanged on failure
}

static void test_string_null_and_empty_rejected() {
    int out = 42;
    TEST_ASSERT_FALSE(from_string(nullptr, &out));
    TEST_ASSERT_FALSE(from_string("", &out));
    TEST_ASSERT_EQUAL_INT(42, out);
}

static void test_string_null_out_pointer_rejected() {
    TEST_ASSERT_FALSE(from_string("info", nullptr));
}

static void test_int_valid_range() {
    TEST_ASSERT_TRUE(is_valid_int(0));
    TEST_ASSERT_TRUE(is_valid_int(1));
    TEST_ASSERT_TRUE(is_valid_int(2));
    TEST_ASSERT_TRUE(is_valid_int(3));
    TEST_ASSERT_TRUE(is_valid_int(4));
    TEST_ASSERT_TRUE(is_valid_int(5));
}

static void test_int_out_of_range_rejected() {
    TEST_ASSERT_FALSE(is_valid_int(-1));
    TEST_ASSERT_FALSE(is_valid_int(6));
    TEST_ASSERT_FALSE(is_valid_int(100));
    TEST_ASSERT_FALSE(is_valid_int(-100));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_string_known_tokens_accepted);
    RUN_TEST(test_string_unknown_rejected);
    RUN_TEST(test_string_null_and_empty_rejected);
    RUN_TEST(test_string_null_out_pointer_rejected);
    RUN_TEST(test_int_valid_range);
    RUN_TEST(test_int_out_of_range_rejected);
    return UNITY_END();
}
