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

// --- hash-store parity (the open-addressed rewrite must behave exactly like
// the old linear-scan store through the public API) ---

// Fill to CAP with realistic dotted paths and read every one back. Exercises
// FNV-1a distribution + linear probing at full load (160/256 slots).
static void test_hash_all_entries_retrievable_at_capacity() {
    static sk::PathStore s;  // ~15 KB: keep off the host stack for parity with device use
    s.clear();
    char buf[64];
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "electrical.batteries.%d.voltage", i);
        TEST_ASSERT_TRUE(s.set(buf, i * 0.5));
    }
    TEST_ASSERT_EQUAL_INT(sk::PathStore::CAP, s.size());
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "electrical.batteries.%d.voltage", i);
        TEST_ASSERT_TRUE(s.has(buf));
        TEST_ASSERT_EQUAL_DOUBLE(i * 0.5, s.get(buf));
    }
    // A never-stored sibling path must still miss (probe terminates cleanly).
    TEST_ASSERT_FALSE(s.has("electrical.batteries.house.current"));
    TEST_ASSERT_TRUE(isnan(s.get("electrical.batteries.house.current")));
}

// Updates at capacity must not consume new slots or disturb neighbors.
static void test_hash_update_at_capacity_keeps_size() {
    static sk::PathStore s;
    s.clear();
    char buf[32];
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "path.%d", i);
        TEST_ASSERT_TRUE(s.set(buf, i));
    }
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "path.%d", i);
        TEST_ASSERT_TRUE(s.set(buf, i + 1000));
    }
    TEST_ASSERT_EQUAL_INT(sk::PathStore::CAP, s.size());
    TEST_ASSERT_EQUAL_DOUBLE(1000, s.get("path.0"));
    TEST_ASSERT_EQUAL_DOUBLE(1000 + sk::PathStore::CAP - 1,
                             s.get("path.159"));  // CAP == 160
}

// clear() must reset the hash table too - stale slot indices after a clear
// would resolve new keys to old entries.
static void test_hash_clear_then_reinsert() {
    static sk::PathStore s;
    s.clear();
    char buf[32];
    for (int i = 0; i < sk::PathStore::CAP; ++i) {
        snprintf(buf, sizeof(buf), "old.%d", i);
        s.set(buf, i);
    }
    s.clear();
    TEST_ASSERT_EQUAL_INT(0, s.size());
    TEST_ASSERT_FALSE(s.has("old.0"));
    TEST_ASSERT_TRUE(s.set("new.path", 42.0));
    TEST_ASSERT_EQUAL_DOUBLE(42.0, s.get("new.path"));
    TEST_ASSERT_EQUAL_INT(1, s.size());
}

// Over-long paths (>= PATH_LEN) are stored truncated and dedupe on the
// truncated key: repeated sets of the same long path update ONE entry
// (the old linear store leaked a duplicate entry per set).
static void test_hash_long_path_truncates_and_dedupes() {
    sk::PathStore s;
    char longp[sk::PathStore::PATH_LEN + 40];
    for (size_t i = 0; i < sizeof(longp) - 1; ++i)
        longp[i] = 'a' + (i % 26);
    longp[sizeof(longp) - 1] = 0;
    TEST_ASSERT_TRUE(s.set(longp, 1.0));
    TEST_ASSERT_TRUE(s.set(longp, 2.0));
    TEST_ASSERT_EQUAL_INT(1, s.size());
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s.get(longp));
}

// Dense iteration (insertion order) via path_at/value_at.
static void test_iteration_in_insertion_order() {
    sk::PathStore s;
    s.set("first.path", 1.0);
    s.set("second.path", 2.0);
    s.set("third.path", 3.0);
    TEST_ASSERT_EQUAL_STRING("first.path", s.path_at(0));
    TEST_ASSERT_EQUAL_STRING("second.path", s.path_at(1));
    TEST_ASSERT_EQUAL_STRING("third.path", s.path_at(2));
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s.value_at(1));
    TEST_ASSERT_NULL(s.path_at(3));
    TEST_ASSERT_TRUE(isnan(s.value_at(-1)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_absent_is_nan);
    RUN_TEST(test_set_then_get);
    RUN_TEST(test_upsert_updates_in_place);
    RUN_TEST(test_nan_value_is_stored_distinct_from_absent);
    RUN_TEST(test_full_rejects_new_keeps_updating_existing);
    RUN_TEST(test_clear_empties);
    RUN_TEST(test_hash_all_entries_retrievable_at_capacity);
    RUN_TEST(test_hash_update_at_capacity_keeps_size);
    RUN_TEST(test_hash_clear_then_reinsert);
    RUN_TEST(test_hash_long_path_truncates_and_dedupes);
    RUN_TEST(test_iteration_in_insertion_order);
    return UNITY_END();
}
