// Host tests for the spec 17 §5 recent-errors ring buffer.

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "error_log.h"

void setUp(void) {
    error_log::clear();
}
void tearDown(void) {
    error_log::clear();
}

static void test_starts_empty() {
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)error_log::size());
    error_log::Entry buf[8];
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)error_log::copy(buf, 8));
}

static void test_push_one_then_copy() {
    error_log::push(1000, "boom");
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)error_log::size());
    error_log::Entry buf[8];
    size_t n = error_log::copy(buf, 8);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)n);
    TEST_ASSERT_EQUAL_UINT32(1000, buf[0].timestamp_ms);
    TEST_ASSERT_EQUAL_STRING("boom", buf[0].message);
}

static void test_push_null_and_empty_ignored() {
    error_log::push(1, nullptr);
    error_log::push(2, "");
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)error_log::size());
}

static void test_keeps_chronological_order_until_full() {
    error_log::push(10, "a");
    error_log::push(20, "b");
    error_log::push(30, "c");
    error_log::Entry buf[8];
    size_t n = error_log::copy(buf, 8);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("a", buf[0].message);
    TEST_ASSERT_EQUAL_STRING("b", buf[1].message);
    TEST_ASSERT_EQUAL_STRING("c", buf[2].message);
}

static void test_drops_oldest_on_overflow() {
    // MAX_ENTRIES is 8. Push 10; expect entries 2..9 in order.
    for (uint32_t i = 0; i < 10; ++i) {
        char msg[8];
        snprintf(msg, sizeof(msg), "e%u", (unsigned)i);
        error_log::push(i + 1, msg);
    }
    TEST_ASSERT_EQUAL_UINT(error_log::MAX_ENTRIES, (unsigned)error_log::size());
    error_log::Entry buf[error_log::MAX_ENTRIES];
    size_t n = error_log::copy(buf, error_log::MAX_ENTRIES);
    TEST_ASSERT_EQUAL_UINT(error_log::MAX_ENTRIES, (unsigned)n);
    // First entry should be the third push ("e2" / ts=3).
    TEST_ASSERT_EQUAL_UINT32(3, buf[0].timestamp_ms);
    TEST_ASSERT_EQUAL_STRING("e2", buf[0].message);
    // Last entry should be the latest push ("e9" / ts=10).
    TEST_ASSERT_EQUAL_UINT32(10, buf[error_log::MAX_ENTRIES - 1].timestamp_ms);
    TEST_ASSERT_EQUAL_STRING("e9", buf[error_log::MAX_ENTRIES - 1].message);
}

static void test_oversized_message_is_truncated_safely() {
    char big[error_log::MAX_MESSAGE + 50];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    error_log::push(1, big);
    error_log::Entry buf[1];
    error_log::copy(buf, 1);
    // Always null-terminated, never exceeding MAX_MESSAGE - 1 chars.
    TEST_ASSERT_TRUE(strlen(buf[0].message) <= error_log::MAX_MESSAGE - 1);
}

static void test_copy_cap_smaller_than_size() {
    for (uint32_t i = 0; i < 5; ++i)
        error_log::push(i + 1, "x");
    error_log::Entry buf[2];
    size_t n = error_log::copy(buf, 2);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned)n);
    TEST_ASSERT_EQUAL_UINT32(1, buf[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT32(2, buf[1].timestamp_ms);
}

static void test_clear_resets_state() {
    error_log::push(1, "a");
    error_log::push(2, "b");
    error_log::clear();
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)error_log::size());
    // Subsequent pushes start fresh.
    error_log::push(100, "fresh");
    error_log::Entry buf[8];
    size_t n = error_log::copy(buf, 8);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("fresh", buf[0].message);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_push_one_then_copy);
    RUN_TEST(test_push_null_and_empty_ignored);
    RUN_TEST(test_keeps_chronological_order_until_full);
    RUN_TEST(test_drops_oldest_on_overflow);
    RUN_TEST(test_oversized_message_is_truncated_safely);
    RUN_TEST(test_copy_cap_smaller_than_size);
    RUN_TEST(test_clear_resets_state);
    return UNITY_END();
}
