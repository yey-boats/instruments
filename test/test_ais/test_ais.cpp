// Host unit tests for the AIS target store (include/ais_store.h).
// The store is header-inline pure C++; each test uses a local instance.
//
// NOTE: run with `pio test -e native -f test_ais`. Adding "test_ais" to the
// [env:native] test_filter list in platformio.ini makes it part of the
// default suite (that file is owned by another change).

#include <unity.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ais_store.h"

using ais::Store;
using ais::Target;
using ais::VesselClass;

void setUp(void) {
}
void tearDown(void) {
}

static void test_starts_empty() {
    Store s;
    TEST_ASSERT_EQUAL(0, s.count());
    TEST_ASSERT_EQUAL(-1, s.find(123u));
    Target t;
    TEST_ASSERT_FALSE(s.get(0, t));
    TEST_ASSERT_EQUAL(0, s.snapshot(&t, 1, 1000));
}

static void test_upsert_position_full_report() {
    Store s;
    int i = s.upsert_position(244813000u, 41.31, 2.18, 6.2f, 1.5f, 1.6f, VesselClass::ClassA, 5000);
    TEST_ASSERT_EQUAL(0, i);
    TEST_ASSERT_EQUAL(1, s.count());
    Target t;
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_EQUAL_UINT32(244813000u, t.mmsi);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 41.31, t.lat_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.18, t.lon_deg);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 6.2f, t.sog_mps);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.5f, t.cog_rad);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.6f, t.heading_rad);
    TEST_ASSERT_EQUAL(static_cast<int>(VesselClass::ClassA), static_cast<int>(t.cls));
    TEST_ASSERT_EQUAL_UINT32(5000, t.last_seen_ms);
    // MMSI 0 rejected.
    TEST_ASSERT_EQUAL(-1, s.upsert_position(0, 1, 2, 3, 4, 5, VesselClass::ClassA, 1));
}

static void test_upsert_dedups_by_mmsi_and_merges_nan() {
    Store s;
    // SignalK-style per-path arrival: position, then SOG, then COG.
    s.upsert_position(244813000u, 41.31, 2.18, NAN, NAN, NAN, VesselClass::Unknown, 1000);
    s.upsert_position(244813000u, NAN, NAN, 6.2f, NAN, NAN, VesselClass::Unknown, 1100);
    s.upsert_position(244813000u, NAN, NAN, NAN, 1.5f, NAN, VesselClass::Unknown, 1200);
    TEST_ASSERT_EQUAL(1, s.count());
    Target t;
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 41.31, t.lat_deg);  // survived the NaN merges
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 6.2f, t.sog_mps);
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.5f, t.cog_rad);
    TEST_ASSERT_TRUE(std::isnan(t.heading_rad));  // never reported
    TEST_ASSERT_EQUAL_UINT32(1200, t.last_seen_ms);
    // lat/lon only land as a PAIR (a lone latitude must not shear the fix).
    s.upsert_position(244813000u, 42.00, NAN, NAN, NAN, NAN, VesselClass::Unknown, 1300);
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 41.31, t.lat_deg);
}

static void test_upsert_static_merges_name_and_class() {
    Store s;
    // Static data may arrive BEFORE any position report.
    s.upsert_static(244813000u, "MV NORDLYS", VesselClass::ClassA, 1000);
    TEST_ASSERT_EQUAL(1, s.count());
    // Position merge keeps the name; Unknown class never downgrades.
    s.upsert_position(244813000u, 41.3, 2.2, 5.0f, NAN, NAN, VesselClass::Unknown, 2000);
    Target t;
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_EQUAL_STRING("MV NORDLYS", t.name);
    TEST_ASSERT_EQUAL(static_cast<int>(VesselClass::ClassA), static_cast<int>(t.cls));
    // Empty name keeps the stored one; a new name replaces it.
    s.upsert_static(244813000u, "", VesselClass::Unknown, 2500);
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_EQUAL_STRING("MV NORDLYS", t.name);
    s.upsert_static(244813000u, "RENAMED", VesselClass::Unknown, 2600);
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_EQUAL_STRING("RENAMED", t.name);
    // Overlong names truncate safely into name[24].
    s.upsert_static(244813000u, "AN EXTREMELY LONG VESSEL NAME", VesselClass::Unknown, 2700);
    TEST_ASSERT_TRUE(s.get(0, t));
    TEST_ASSERT_EQUAL(sizeof(t.name) - 1, strlen(t.name));
}

static void test_age_out_drops_stale_targets() {
    Store s;
    s.upsert_position(111111111u, 41.0, 2.0, NAN, NAN, NAN, VesselClass::ClassB, 1000);
    s.upsert_position(222222222u, 41.1, 2.1, NAN, NAN, NAN, VesselClass::ClassB, 300000);
    // At t=370000: target 1 is 369 s old (> 6 min) -> dropped; target 2 (70 s)
    // survives.
    TEST_ASSERT_EQUAL(1, s.age_out(370000));
    TEST_ASSERT_EQUAL(1, s.count());
    TEST_ASSERT_EQUAL(-1, s.find(111111111u));
    TEST_ASSERT_TRUE(s.find(222222222u) >= 0);
    // Boundary: exactly MAX_AGE_MS old is kept (drop is strictly older).
    Store s2;
    s2.upsert_position(333333333u, 1, 2, NAN, NAN, NAN, VesselClass::Unknown, 1000);
    TEST_ASSERT_EQUAL(0, s2.age_out(1000 + Store::MAX_AGE_MS));
    TEST_ASSERT_EQUAL(1, s2.age_out(1001 + Store::MAX_AGE_MS));
}

static void test_snapshot_copies_live_targets_only() {
    Store s;
    s.upsert_position(111111111u, 41.0, 2.0, NAN, NAN, NAN, VesselClass::ClassA, 1000);
    s.upsert_position(222222222u, 41.1, 2.1, NAN, NAN, NAN, VesselClass::ClassB, 350000);
    Target out[4];
    // Aged target filtered from the snapshot even before age_out() runs.
    int n = s.snapshot(out, 4, 400000);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_UINT32(222222222u, out[0].mmsi);
    TEST_ASSERT_EQUAL(2, s.count());  // snapshot does not mutate
    // max cap respected.
    s.upsert_position(333333333u, 41.2, 2.2, NAN, NAN, NAN, VesselClass::ClassB, 350000);
    n = s.snapshot(out, 1, 351000);
    TEST_ASSERT_EQUAL(1, n);
}

static void test_capacity_evicts_oldest_target() {
    Store s;
    for (int i = 0; i < Store::CAP; ++i) {
        // Ascending last_seen: target 0 is the stalest.
        s.upsert_position(100000000u + i, 41.0, 2.0, NAN, NAN, NAN, VesselClass::ClassB,
                          1000 + i * 10);
    }
    TEST_ASSERT_EQUAL(Store::CAP, s.count());
    // Refresh target 0 so target 1 becomes the stalest.
    s.upsert_position(100000000u, 41.5, 2.5, NAN, NAN, NAN, VesselClass::ClassB, 9000);
    int i = s.upsert_position(999999999u, 57.0, 11.0, NAN, NAN, NAN, VesselClass::ClassA, 10000);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(Store::CAP, s.count());   // pool stays at cap
    TEST_ASSERT_EQUAL(-1, s.find(100000001u));  // stalest evicted
    TEST_ASSERT_TRUE(s.find(999999999u) >= 0);  // newcomer present
    TEST_ASSERT_TRUE(s.find(100000000u) >= 0);  // refreshed one kept
    // The evicted slot was fully reset (no stale name/class bleed-through).
    Target t;
    TEST_ASSERT_TRUE(s.get(s.find(999999999u), t));
    TEST_ASSERT_EQUAL_STRING("", t.name);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 57.0, t.lat_deg);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_upsert_position_full_report);
    RUN_TEST(test_upsert_dedups_by_mmsi_and_merges_nan);
    RUN_TEST(test_upsert_static_merges_name_and_class);
    RUN_TEST(test_age_out_drops_stale_targets);
    RUN_TEST(test_snapshot_copies_live_targets_only);
    RUN_TEST(test_capacity_evicts_oldest_target);
    return UNITY_END();
}
