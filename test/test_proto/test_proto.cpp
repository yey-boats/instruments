#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "proto/proto.h"

using namespace proto;

void setUp() {
}
void tearDown() {
}

static void test_version_parse_and_compat() {
    int M, m;
    TEST_ASSERT_TRUE(parse_version("1.7", M, m));
    TEST_ASSERT_EQUAL_INT(1, M);
    TEST_ASSERT_EQUAL_INT(7, m);
    TEST_ASSERT_FALSE(parse_version("x", M, m));
    TEST_ASSERT_TRUE(version_compatible("1.0"));
    TEST_ASSERT_TRUE(version_compatible("1.99"));  // higher minor ok
    TEST_ASSERT_FALSE(version_compatible("2.0"));  // major mismatch
}

static void test_auth() {
    TEST_ASSERT_TRUE(auth_ok("", "anything"));  // open
    TEST_ASSERT_TRUE(auth_ok("hunter2", "hunter2"));
    TEST_ASSERT_FALSE(auth_ok("hunter2", "nope"));
    TEST_ASSERT_FALSE(auth_ok("hunter2", nullptr));
}

static void test_attach_fixture_roundtrips() {
    const char *json =
        R"({"v":"1.0","t":"attach","controllerId":"knob-aa01","name":"Helm knob","color":"#00bcd4","key":"hunter2","ttlMs":10000})";
    JsonDocument d;
    deserializeJson(d, json);
    Attach a;
    from_json(d.as<JsonObjectConst>(), a);
    TEST_ASSERT_EQUAL_STRING("knob-aa01", a.controllerId);
    TEST_ASSERT_EQUAL_STRING("#00bcd4", a.color);
    TEST_ASSERT_EQUAL_INT(10000, (int)a.ttlMs);
    JsonDocument out;
    to_json(out.to<JsonObject>(), a);
    TEST_ASSERT_EQUAL_STRING("attach", out["t"]);
    TEST_ASSERT_EQUAL_STRING("knob-aa01", out["controllerId"]);
}

static void test_device_record_views_roundtrip() {
    const char *json =
        R"({"v":"1.0","deviceId":"mfd-helm","role":"both","currentView":"wind","views":[{"id":"wind","title":"Wind"},{"id":"nav","title":"Nav"}],"transports":["ip","ble"]})";
    JsonDocument d;
    deserializeJson(d, json);
    DeviceRecord r;
    from_json(d.as<JsonObjectConst>(), r);
    TEST_ASSERT_EQUAL_INT(2, r.views_count);
    TEST_ASSERT_EQUAL_STRING("nav", r.views[1].id);
    TEST_ASSERT_EQUAL_INT(2, r.transports_count);
    TEST_ASSERT_EQUAL_STRING("ble", r.transports[1]);
}

static void test_unknown_field_ignored() {
    const char *json =
        R"({"v":"1.7","t":"attach","controllerId":"x","name":"x","color":"#010203","futureField":{"nested":true}})";
    JsonDocument d;
    deserializeJson(d, json);
    Attach a;
    from_json(d.as<JsonObjectConst>(), a);
    TEST_ASSERT_EQUAL_STRING("x", a.controllerId);  // parsed despite extra field
    TEST_ASSERT_TRUE(version_compatible(a.v));      // 1.7 compatible
}

static void test_session_table_lifecycle() {
    SessionTable t;
    t.clear();
    Attach a;
    strcpy(a.v, "1.0");
    strcpy(a.controllerId, "c1");
    strcpy(a.name, "C1");
    strcpy(a.color, "#00bcd4");
    char sid[16];
    int idx = t.attach(a, 1000, sid, sizeof(sid));
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_INT(1, t.active_count());
    TEST_ASSERT_TRUE(t.heartbeat(sid, 5000));
    TEST_ASSERT_EQUAL_INT(0, t.reap(9000, kDefaultTtlMs));   // 5000+10000 > 9000, alive
    TEST_ASSERT_EQUAL_INT(1, t.reap(20000, kDefaultTtlMs));  // stale -> reaped
    TEST_ASSERT_EQUAL_INT(0, t.active_count());
}

static void test_control_state_serialization() {
    SessionTable t;
    t.clear();
    Attach a;
    strcpy(a.v, "1.0");
    strcpy(a.controllerId, "c1");
    strcpy(a.name, "Knob");
    strcpy(a.color, "#00bcd4");
    char sid[16];
    t.attach(a, 1000, sid, sizeof(sid));
    ControlState cs;
    t.to_control_state(cs, "nav");
    JsonDocument out;
    to_json(out.to<JsonObject>(), cs);
    TEST_ASSERT_EQUAL_STRING("controlState", out["t"]);
    TEST_ASSERT_EQUAL_STRING("nav", out["currentView"]);
    TEST_ASSERT_EQUAL_STRING("#00bcd4", out["sessions"][0]["color"]);
}

static proto::Attach mk_attach(const char *id, const char *name, const char *color) {
    proto::Attach a{};
    strncpy(a.v, "1.0", sizeof(a.v) - 1);
    strncpy(a.controllerId, id, sizeof(a.controllerId) - 1);
    strncpy(a.name, name, sizeof(a.name) - 1);
    strncpy(a.color, color, sizeof(a.color) - 1);
    return a;
}

static void test_attach_full_table_returns_minus_one() {
    SessionTable t;
    t.clear();
    char sid[16];
    for (int i = 0; i < kMaxSessions; ++i) {
        proto::Attach a = mk_attach("cx", "CX", "#aabbcc");
        int idx = t.attach(a, 1000 + i, sid, sizeof(sid));
        TEST_ASSERT_TRUE(idx >= 0);
    }
    TEST_ASSERT_EQUAL_INT(kMaxSessions, t.active_count());
    proto::Attach extra = mk_attach("overflow", "Overflow", "#ffffff");
    int overflow_idx = t.attach(extra, 9000, sid, sizeof(sid));
    TEST_ASSERT_EQUAL_INT(-1, overflow_idx);
    TEST_ASSERT_EQUAL_INT(kMaxSessions, t.active_count());
}

static void test_most_recent_idx_last_writer() {
    SessionTable t;
    t.clear();
    char sid1[16], sid2[16];
    proto::Attach a1 = mk_attach("c1", "C1", "#111111");
    int idx1 = t.attach(a1, 1000, sid1, sizeof(sid1));
    TEST_ASSERT_TRUE(idx1 >= 0);
    proto::Attach a2 = mk_attach("c2", "C2", "#222222");
    int idx2 = t.attach(a2, 2000, sid2, sizeof(sid2));
    TEST_ASSERT_TRUE(idx2 >= 0);
    TEST_ASSERT_EQUAL_INT(idx2, (int)t.mostRecentIdx);
    TEST_ASSERT_TRUE(t.heartbeat(sid1, 3000));
    TEST_ASSERT_EQUAL_INT(idx1, (int)t.mostRecentIdx);
}

static void test_detach_removes_session() {
    SessionTable t;
    t.clear();
    char sid[16];
    proto::Attach a = mk_attach("c1", "C1", "#aabbcc");
    t.attach(a, 1000, sid, sizeof(sid));
    TEST_ASSERT_EQUAL_INT(1, t.active_count());
    TEST_ASSERT_TRUE(t.detach(sid));
    TEST_ASSERT_EQUAL_INT(0, t.active_count());
    TEST_ASSERT_FALSE(t.detach("nope"));
}

static void test_reap_boundary() {
    SessionTable t;
    t.clear();
    char sid[16];
    proto::Attach a = mk_attach("c1", "C1", "#aabbcc");
    t.attach(a, 1000, sid, sizeof(sid));
    // Exactly at TTL boundary: now - lastSeen == ttl, not > ttl, so survives.
    TEST_ASSERT_EQUAL_INT(0, t.reap(1000 + kDefaultTtlMs, kDefaultTtlMs));
    // One ms beyond: now - lastSeen > ttl, gets reaped.
    TEST_ASSERT_EQUAL_INT(1, t.reap(1000 + kDefaultTtlMs + 1, kDefaultTtlMs));
    TEST_ASSERT_EQUAL_INT(0, t.active_count());
}

static void test_control_state_multi_session() {
    SessionTable t;
    t.clear();
    char sid1[16], sid2[16];
    proto::Attach a1 = mk_attach("c1", "C1", "#111111");
    t.attach(a1, 1000, sid1, sizeof(sid1));
    proto::Attach a2 = mk_attach("c2", "C2", "#222222");
    t.attach(a2, 2000, sid2, sizeof(sid2));
    ControlState cs{};
    t.to_control_state(cs, "wind");
    TEST_ASSERT_EQUAL_INT(2, cs.sessions_count);
    TEST_ASSERT_EQUAL_STRING("wind", cs.currentView);
    TEST_ASSERT_EQUAL_STRING("1.0", cs.v);
    bool found111 = false, found222 = false;
    for (int i = 0; i < cs.sessions_count; ++i) {
        if (strcmp(cs.sessions[i].color, "#111111") == 0) found111 = true;
        if (strcmp(cs.sessions[i].color, "#222222") == 0) found222 = true;
    }
    TEST_ASSERT_TRUE(found111);
    TEST_ASSERT_TRUE(found222);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_version_parse_and_compat);
    RUN_TEST(test_auth);
    RUN_TEST(test_attach_fixture_roundtrips);
    RUN_TEST(test_device_record_views_roundtrip);
    RUN_TEST(test_unknown_field_ignored);
    RUN_TEST(test_session_table_lifecycle);
    RUN_TEST(test_control_state_serialization);
    RUN_TEST(test_attach_full_table_returns_minus_one);
    RUN_TEST(test_most_recent_idx_last_writer);
    RUN_TEST(test_detach_removes_session);
    RUN_TEST(test_reap_boundary);
    RUN_TEST(test_control_state_multi_session);
    return UNITY_END();
}
