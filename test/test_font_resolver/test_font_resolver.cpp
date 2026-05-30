#include <unity.h>

#include "font_resolver.h"

using namespace font_resolver;

void setUp(void) {
}
void tearDown(void) {
}

static const uint16_t SIZES[] = {14, 20, 28, 48};
static constexpr size_t N = sizeof(SIZES) / sizeof(SIZES[0]);

static void test_exact_match_returned() {
    TEST_ASSERT_EQUAL_UINT16(14, resolve(14, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(20, resolve(20, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(28, resolve(28, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(48, resolve(48, SIZES, N));
}

static void test_between_picks_nearest_lower() {
    // Spec 19: nearest-lower (not nearest-absolute) to avoid overflow.
    TEST_ASSERT_EQUAL_UINT16(20, resolve(24, SIZES, N));  // 20 < 24 < 28
    TEST_ASSERT_EQUAL_UINT16(20, resolve(22, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(28, resolve(34, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(28, resolve(47, SIZES, N));  // very close to 48 but still picks 28
}

static void test_below_smallest_returns_smallest() {
    TEST_ASSERT_EQUAL_UINT16(14, resolve(8, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(14, resolve(13, SIZES, N));
}

static void test_above_largest_returns_largest() {
    TEST_ASSERT_EQUAL_UINT16(48, resolve(60, SIZES, N));
    TEST_ASSERT_EQUAL_UINT16(48, resolve(200, SIZES, N));
}

static void test_empty_table_returns_requested() {
    TEST_ASSERT_EQUAL_UINT16(42, resolve(42, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT16(42, resolve(42, SIZES, 0));
}

static void test_single_entry_table() {
    static const uint16_t one[] = {20};
    TEST_ASSERT_EQUAL_UINT16(20, resolve(10, one, 1));
    TEST_ASSERT_EQUAL_UINT16(20, resolve(20, one, 1));
    TEST_ASSERT_EQUAL_UINT16(20, resolve(60, one, 1));
}

static void test_default_resolve() {
    // Convenience accessor uses DEFAULT_SIZES.
    TEST_ASSERT_EQUAL_UINT16(14, resolve_default(10));
    TEST_ASSERT_EQUAL_UINT16(28, resolve_default(36));  // nearest-lower
    TEST_ASSERT_EQUAL_UINT16(48, resolve_default(100));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_exact_match_returned);
    RUN_TEST(test_between_picks_nearest_lower);
    RUN_TEST(test_below_smallest_returns_smallest);
    RUN_TEST(test_above_largest_returns_largest);
    RUN_TEST(test_empty_table_returns_requested);
    RUN_TEST(test_single_entry_table);
    RUN_TEST(test_default_resolve);
    return UNITY_END();
}
