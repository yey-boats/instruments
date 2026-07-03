// Host unit tests for the notifications/alarms store (include/notifications.h).
// The store is header-inline pure C++; each test uses a local instance.
//
// NOTE: run with `pio test -e native -f test_notifications`. Adding
// "test_notifications" to the [env:native] test_filter list in platformio.ini
// makes it part of the default suite (that file is owned by another change).

#include <unity.h>
#include <cstdio>
#include <cstring>

#include "notifications.h"

using notif::Entry;
using notif::State;
using notif::Store;

void setUp(void) {
}
void tearDown(void) {
}

static void test_starts_empty() {
    Store s;
    TEST_ASSERT_EQUAL(0, s.count());
    TEST_ASSERT_EQUAL(static_cast<int>(State::Normal),
                      static_cast<int>(s.highest_active_severity()));
    Entry e;
    TEST_ASSERT_FALSE(s.get(0, e));
}

static void test_upsert_inserts_and_updates() {
    Store s;
    int i = s.upsert("navigation.anchor", State::Warn, "Radius 40m", notif::METHOD_VISUAL, 1000);
    TEST_ASSERT_EQUAL(0, i);
    TEST_ASSERT_EQUAL(1, s.count());
    Entry e;
    TEST_ASSERT_TRUE(s.get(0, e));
    TEST_ASSERT_EQUAL_STRING("navigation.anchor", e.path);
    TEST_ASSERT_EQUAL_STRING("Radius 40m", e.message);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Warn), static_cast<int>(e.state));
    TEST_ASSERT_EQUAL_UINT8(notif::METHOD_VISUAL, e.method);
    TEST_ASSERT_EQUAL_UINT32(1000, e.first_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, e.last_ms);
    // Re-assert same path: updates in place (no duplicate), keeps first_ms.
    i = s.upsert("navigation.anchor", State::Warn, "Radius 42m", notif::METHOD_VISUAL, 3000);
    TEST_ASSERT_EQUAL(0, i);
    TEST_ASSERT_EQUAL(1, s.count());
    TEST_ASSERT_TRUE(s.get(0, e));
    TEST_ASSERT_EQUAL_STRING("Radius 42m", e.message);
    TEST_ASSERT_EQUAL_UINT32(1000, e.first_ms);
    TEST_ASSERT_EQUAL_UINT32(3000, e.last_ms);
}

static void test_normal_state_clears_entry() {
    Store s;
    s.upsert("environment.depth", State::Alarm, "Shallow", notif::METHOD_SOUND, 1000);
    s.upsert("navigation.anchor", State::Warn, "Drift", notif::METHOD_VISUAL, 1000);
    TEST_ASSERT_EQUAL(2, s.count());
    int i = s.upsert("environment.depth", State::Normal, "ok", 0, 2000);
    TEST_ASSERT_EQUAL(-1, i);
    TEST_ASSERT_EQUAL(1, s.count());
    Entry e;
    TEST_ASSERT_TRUE(s.get(0, e));
    TEST_ASSERT_EQUAL_STRING("navigation.anchor", e.path);
    // Clearing an unknown path is a no-op.
    s.upsert("no.such.alarm", State::Normal, nullptr, 0, 2000);
    TEST_ASSERT_EQUAL(1, s.count());
}

static void test_highest_active_severity_orders() {
    Store s;
    s.upsert("a", State::Alert, "a", 0, 100);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Alert),
                      static_cast<int>(s.highest_active_severity()));
    s.upsert("b", State::Emergency, "b", 0, 100);
    s.upsert("c", State::Warn, "c", 0, 100);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Emergency),
                      static_cast<int>(s.highest_active_severity()));
    // Clearing the emergency drops the highest back down.
    s.upsert("b", State::Normal, nullptr, 0, 200);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Warn), static_cast<int>(s.highest_active_severity()));
}

static void test_acknowledge_silences_active_severity() {
    Store s;
    int i = s.upsert("autopilot", State::Alarm, "Off course", notif::METHOD_SOUND, 100);
    TEST_ASSERT_TRUE(s.acknowledge(i));
    // Acked alarms no longer drive the active severity...
    TEST_ASSERT_EQUAL(static_cast<int>(State::Normal),
                      static_cast<int>(s.highest_active_severity()));
    // ...but stay listed with their real state.
    TEST_ASSERT_EQUAL(static_cast<int>(State::Alarm), static_cast<int>(s.highest_severity()));
    Entry e;
    TEST_ASSERT_TRUE(s.get(i, e));
    TEST_ASSERT_TRUE(e.acknowledged);
    // Re-assert at the SAME state: stays acknowledged (no re-alert spam).
    s.upsert("autopilot", State::Alarm, "Off course", notif::METHOD_SOUND, 5000);
    TEST_ASSERT_TRUE(s.get(i, e));
    TEST_ASSERT_TRUE(e.acknowledged);
    // Escalation to a HIGHER state clears the ack (crew must re-alert).
    s.upsert("autopilot", State::Emergency, "Off course!", notif::METHOD_SOUND, 6000);
    TEST_ASSERT_TRUE(s.get(i, e));
    TEST_ASSERT_FALSE(e.acknowledged);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Emergency),
                      static_cast<int>(s.highest_active_severity()));
    // Ack-by-path works too.
    TEST_ASSERT_TRUE(s.acknowledge("autopilot"));
    TEST_ASSERT_FALSE(s.acknowledge("nope"));
    TEST_ASSERT_FALSE(s.acknowledge(99));
}

static void test_expire_drops_stale_entries() {
    Store s;
    s.upsert("old", State::Warn, "old", 0, 1000);
    s.upsert("fresh", State::Alert, "fresh", 0, 500000);
    // 1000 + 10 min < 700000 -> "old" expires; "fresh" survives.
    int removed = s.expire(700000);
    TEST_ASSERT_EQUAL(1, removed);
    TEST_ASSERT_EQUAL(1, s.count());
    Entry e;
    TEST_ASSERT_TRUE(s.get(0, e));
    TEST_ASSERT_EQUAL_STRING("fresh", e.path);
    // Custom horizon.
    TEST_ASSERT_EQUAL(1, s.expire(510000, 5000));
    TEST_ASSERT_EQUAL(0, s.count());
}

static void test_capacity_evicts_lowest_priority() {
    Store s;
    char path[16];
    for (int i = 0; i < Store::CAP; ++i) {
        snprintf(path, sizeof(path), "alarm.%d", i);
        s.upsert(path, State::Alarm, "x", 0, 1000 + i);
    }
    TEST_ASSERT_EQUAL(Store::CAP, s.count());
    // Ack one entry: it becomes the eviction candidate.
    TEST_ASSERT_TRUE(s.acknowledge("alarm.7"));
    int i = s.upsert("newcomer", State::Emergency, "boom", 0, 5000);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(Store::CAP, s.count());     // still at cap
    TEST_ASSERT_FALSE(s.acknowledge("alarm.7"));  // evicted
    TEST_ASSERT_TRUE(s.acknowledge("newcomer"));  // present
    // A newcomer BELOW everything stored is dropped instead.
    i = s.upsert("weakling", State::Alert, "meh", 0, 6000);
    TEST_ASSERT_EQUAL(-1, i);
    TEST_ASSERT_FALSE(s.acknowledge("weakling"));
}

static void test_state_string_bridges() {
    TEST_ASSERT_EQUAL(static_cast<int>(State::Normal),
                      static_cast<int>(notif::state_from_string("normal")));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Normal),
                      static_cast<int>(notif::state_from_string("nominal")));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Normal),
                      static_cast<int>(notif::state_from_string(nullptr)));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Alert),
                      static_cast<int>(notif::state_from_string("alert")));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Warn),
                      static_cast<int>(notif::state_from_string("warn")));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Alarm),
                      static_cast<int>(notif::state_from_string("alarm")));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Emergency),
                      static_cast<int>(notif::state_from_string("emergency")));
    // Unknown state string: conservatively Alert, never silently dropped.
    TEST_ASSERT_EQUAL(static_cast<int>(State::Alert),
                      static_cast<int>(notif::state_from_string("wat")));
    TEST_ASSERT_EQUAL_STRING("alarm", notif::state_name(State::Alarm));
}

static void test_long_strings_truncate_safely() {
    Store s;
    char longpath[128];
    memset(longpath, 'p', sizeof(longpath));
    longpath[sizeof(longpath) - 1] = 0;
    char longmsg[256];
    memset(longmsg, 'm', sizeof(longmsg));
    longmsg[sizeof(longmsg) - 1] = 0;
    int i = s.upsert(longpath, State::Warn, longmsg, 0, 100);
    TEST_ASSERT_TRUE(i >= 0);
    Entry e;
    TEST_ASSERT_TRUE(s.get(i, e));
    TEST_ASSERT_EQUAL(sizeof(e.path) - 1, strlen(e.path));
    TEST_ASSERT_EQUAL(sizeof(e.message) - 1, strlen(e.message));
    // Upsert with the same (truncated) key dedups.
    s.upsert(longpath, State::Warn, "short", 0, 200);
    TEST_ASSERT_EQUAL(1, s.count());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_upsert_inserts_and_updates);
    RUN_TEST(test_normal_state_clears_entry);
    RUN_TEST(test_highest_active_severity_orders);
    RUN_TEST(test_acknowledge_silences_active_severity);
    RUN_TEST(test_expire_drops_stale_entries);
    RUN_TEST(test_capacity_evicts_lowest_priority);
    RUN_TEST(test_state_string_bridges);
    RUN_TEST(test_long_strings_truncate_safely);
    return UNITY_END();
}
