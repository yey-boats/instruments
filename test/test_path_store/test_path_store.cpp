#include "path_store.h"

#include <math.h>
#include <stdio.h>
#include <unity.h>

void setUp() {
}
void tearDown() {
}

static void test_absent_is_nan() {
    sk::PathStore s;
    TEST_ASSERT_TRUE(isnan(s.get("environment.depth.belowTransducer")));
    TEST_ASSERT_FALSE(s.has("anything"));
    TEST_ASSERT_EQUAL_INT(0, s.size());
}

static void test_set_then_get() {
    sk::PathStore s;
    TEST_ASSERT_TRUE(s.set("navigation.speedOverGround", 3.5));
    TEST_ASSERT_TRUE(s.has("navigation.speedOverGround"));
    TEST_ASSERT_EQUAL_DOUBLE(3.5, s.get("navigation.speedOverGround"));
    TEST_ASSERT_EQUAL_INT(1, s.size());
}

static void test_upsert_updates_in_place() {
    sk::PathStore s;
    s.set("p", 1.0);
    s.set("p", 2.0);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s.get("p"));
    TEST_ASSERT_EQUAL_INT(1, s.size());  // not duplicated
}

static void test_nan_value_is_stored_distinct_from_absent() {
    sk::PathStore s;
    s.set("p", NAN);
    TEST_ASSERT_TRUE(s.has("p"));         // present...
    TEST_ASSERT_TRUE(isnan(s.get("p")));  // ...but NaN
}

static void test_full_rejects_new_keeps_updating_existing() {
    sk::PathStore s;
    char buf[16];
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "p%d", i);
        TEST_ASSERT_TRUE(s.set(buf, i));
    }
    TEST_ASSERT_FALSE(s.set("overflow", 9));  // new key rejected when full
    TEST_ASSERT_TRUE(s.set("p0", 100));       // existing key still updates
    TEST_ASSERT_EQUAL_DOUBLE(100, s.get("p0"));
}

static void test_clear_empties() {
    sk::PathStore s;
    s.set("p", 1.0);
    s.clear();
    TEST_ASSERT_EQUAL_INT(0, s.size());
    TEST_ASSERT_FALSE(s.has("p"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_absent_is_nan);
    RUN_TEST(test_set_then_get);
    RUN_TEST(test_upsert_updates_in_place);
    RUN_TEST(test_nan_value_is_stored_distinct_from_absent);
    RUN_TEST(test_full_rejects_new_keeps_updating_existing);
    RUN_TEST(test_clear_empties);
    return UNITY_END();
}
