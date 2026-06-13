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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_version_parse_and_compat);
    RUN_TEST(test_auth);
    RUN_TEST(test_attach_fixture_roundtrips);
    RUN_TEST(test_device_record_views_roundtrip);
    RUN_TEST(test_unknown_field_ignored);
    RUN_TEST(test_session_table_lifecycle);
    RUN_TEST(test_control_state_serialization);
    return UNITY_END();
}
