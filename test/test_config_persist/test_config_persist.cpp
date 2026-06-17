// Host tests for the Slice 4 flash-persistent-config decision logic.
// These pin the persist / boot-apply / drift-refetch rules so a refactor
// of manager.cpp can't quietly re-burn flash on every apply, mis-apply a
// stale-schema blob on boot, or re-fetch an unchanged config after reboot.
//
// Project: esp32-boat-mfd. (c) navado and contributors.

#include <unity.h>

#include "config_persist.h"

using namespace config_persist;

void setUp(void) {
}
void tearDown(void) {
}

// ---- should_persist -----------------------------------------------------

static void test_persist_on_changed_hash() {
    // Applied ok, new non-empty hash differs from persisted -> write.
    TEST_ASSERT_TRUE(should_persist(true, "abc123", "old999"));
    TEST_ASSERT_TRUE(should_persist(true, "abc123", ""));  // nothing persisted yet
}

static void test_no_persist_when_hash_unchanged() {
    // Same hash already on flash -> skip the write (flash-wear rule).
    TEST_ASSERT_FALSE(should_persist(true, "abc123", "abc123"));
}

static void test_no_persist_when_apply_failed() {
    // Partial apply -> never persist, even with a changed hash.
    TEST_ASSERT_FALSE(should_persist(false, "abc123", "old999"));
}

static void test_no_persist_when_hash_empty() {
    // Manager advertised no hash -> nothing stable to key the blob by.
    TEST_ASSERT_FALSE(should_persist(true, "", "old999"));
    TEST_ASSERT_FALSE(should_persist(true, "", ""));
}

// ---- should_apply_on_boot ----------------------------------------------

static void test_boot_apply_matching_schema() {
    TEST_ASSERT_TRUE(should_apply_on_boot(true, SCHEMA_VERSION, SCHEMA_VERSION));
}

static void test_boot_skip_when_no_blob() {
    TEST_ASSERT_FALSE(should_apply_on_boot(false, SCHEMA_VERSION, SCHEMA_VERSION));
}

static void test_boot_skip_on_schema_mismatch() {
    // Blob from an older schema -> ignore + fall back to defaults.
    TEST_ASSERT_FALSE(should_apply_on_boot(true, 0, SCHEMA_VERSION));
    TEST_ASSERT_FALSE(should_apply_on_boot(true, SCHEMA_VERSION + 1, SCHEMA_VERSION));
}

// ---- should_refetch -----------------------------------------------------

static void test_no_refetch_when_hash_matches() {
    // The critical reboot case: persisted hash == desired hash -> no fetch.
    TEST_ASSERT_FALSE(should_refetch("abc123", "abc123", "v3", "v3"));
}

static void test_refetch_when_hash_differs() {
    TEST_ASSERT_TRUE(should_refetch("def456", "abc123", "v3", "v3"));
}

static void test_refetch_when_version_differs() {
    // Hash empty/equal but version drifts -> still fetch.
    TEST_ASSERT_TRUE(should_refetch("", "", "v4", "v3"));
    TEST_ASSERT_TRUE(should_refetch("abc123", "abc123", "v4", "v3"));
}

static void test_no_refetch_when_manager_advertises_nothing() {
    // Manager sent no desired hash/version -> nothing to compare, no fetch.
    TEST_ASSERT_FALSE(should_refetch("", "abc123", "", "v3"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_persist_on_changed_hash);
    RUN_TEST(test_no_persist_when_hash_unchanged);
    RUN_TEST(test_no_persist_when_apply_failed);
    RUN_TEST(test_no_persist_when_hash_empty);
    RUN_TEST(test_boot_apply_matching_schema);
    RUN_TEST(test_boot_skip_when_no_blob);
    RUN_TEST(test_boot_skip_on_schema_mismatch);
    RUN_TEST(test_no_refetch_when_hash_matches);
    RUN_TEST(test_refetch_when_hash_differs);
    RUN_TEST(test_refetch_when_version_differs);
    RUN_TEST(test_no_refetch_when_manager_advertises_nothing);
    return UNITY_END();
}
