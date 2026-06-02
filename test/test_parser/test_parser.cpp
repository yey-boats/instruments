#include <unity.h>
#include <cmath>
#include <cstring>
#include <string>
#include "signalk_parser.h"

using sk::Data;

void setUp(void) {
}
void tearDown(void) {
}

// Helper: build a delta payload around a single value.
static std::string singleValueDelta(const char *path, const char *valueJson) {
    std::string s = "{\"context\":\"vessels.self\",\"updates\":[{";
    s += "\"values\":[{\"path\":\"";
    s += path;
    s += "\",\"value\":";
    s += valueJson;
    s += "}]}]}";
    return s;
}

static void test_parses_speed_over_ground() {
    Data d;
    auto j = singleValueDelta("navigation.speedOverGround", "3.5");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.5, d.sog);
}

static void test_parses_apparent_wind() {
    Data d;
    auto j = singleValueDelta("environment.wind.angleApparent", "0.785");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.785, d.awa);

    auto j2 = singleValueDelta("environment.wind.speedApparent", "8.2");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 8.2, d.aws);
}

static void test_parses_position_object() {
    Data d;
    auto j = singleValueDelta("navigation.position", "{\"latitude\":41.3851,\"longitude\":2.1734}");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);
    // ArduinoJson stores floats by default - 1e-4 deg ~= 11 m, enough for marine UI.
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 41.3851, d.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 2.1734, d.lon);
}

static void test_parses_depth_variants() {
    Data d;
    auto j1 = singleValueDelta("environment.depth.belowTransducer", "12.3");
    sk::applyDelta(j1.data(), j1.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.3, d.depth);

    Data d2;
    auto j2 = singleValueDelta("environment.depth.belowKeel", "8.5");
    sk::applyDelta(j2.data(), j2.size(), d2);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 8.5, d2.depth);
}

static void test_parses_battery_with_named_bank() {
    Data d;
    auto j = singleValueDelta("electrical.batteries.house.voltage", "12.7");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.7, d.battVoltage);

    auto j2 = singleValueDelta("electrical.batteries.house.stateOfCharge", "0.82");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.82, d.battSoc);
}

static void test_parses_tanks() {
    Data d;
    auto j = singleValueDelta("tanks.fuel.0.currentLevel", "0.65");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.65, d.tankFuel);

    auto j2 = singleValueDelta("tanks.freshWater.starboard.currentLevel", "0.4");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.4, d.tankWater);
}

static void test_unknown_path_is_ignored() {
    Data d;
    auto j = singleValueDelta("some.random.unknown.path", "42");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);              // the value was seen
    TEST_ASSERT_TRUE(std::isnan(d.sog));  // but no known field was set
    TEST_ASSERT_TRUE(std::isnan(d.depth));
}

static void test_malformed_json_returns_error() {
    Data d;
    const char *bad = "{not valid json";
    int n = sk::applyDelta(bad, strlen(bad), d);
    TEST_ASSERT_LESS_THAN(0, n);
}

static void test_keepalive_returns_zero() {
    Data d;
    // SignalK hello/keepalive frames don't contain an "updates" array.
    const char *hello = "{\"name\":\"signalk-server\",\"version\":\"2.27.0\"}";
    int n = sk::applyDelta(hello, strlen(hello), d);
    TEST_ASSERT_EQUAL(0, n);
}

static void test_multiple_values_in_one_delta() {
    Data d;
    const char *j = "{\"context\":\"vessels.self\",\"updates\":[{"
                    "\"values\":["
                    "{\"path\":\"navigation.speedOverGround\",\"value\":4.0},"
                    "{\"path\":\"environment.depth.belowTransducer\",\"value\":7.5},"
                    "{\"path\":\"environment.water.temperature\",\"value\":292.15}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 4.0, d.sog);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 7.5, d.depth);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 292.15, d.waterTemp);
}

static void test_null_value_does_not_overwrite() {
    Data d;
    d.sog = 5.0;  // pre-set
    auto j = singleValueDelta("navigation.speedOverGround", "null");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 5.0, d.sog);  // unchanged
}

static void test_parses_route_fields() {
    Data d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.courseRhumbline.crossTrackError\",\"value\":-42.5},"
                    "{\"path\":\"navigation.courseRhumbline.bearingTrackTrue\",\"value\":1.234},"
                    "{\"path\":\"navigation.courseRhumbline.nextPoint.bearingTrue\",\"value\":1.5},"
                    "{\"path\":\"navigation.courseRhumbline.nextPoint.distance\",\"value\":1234.5},"
                    "{\"path\":\"navigation.courseRhumbline.velocityMadeGood\",\"value\":3.1}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, -42.5, d.xte);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.234, d.cts);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.5, d.btw);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1234.5, d.dtw);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.1, d.vmg);
}

static void test_parses_autopilot_state_and_target() {
    Data d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"steering.autopilot.target.headingTrue\",\"value\":1.234},"
                    "{\"path\":\"steering.autopilot.state\",\"value\":\"auto\"}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.234, d.apTargetHdg);
    TEST_ASSERT_EQUAL_STRING("auto", d.apState);
}

static void test_parses_current() {
    Data d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"environment.current.setTrue\",\"value\":2.0},"
                    "{\"path\":\"environment.current.drift\",\"value\":0.5}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 2.0, d.currentSetTrue);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.5, d.currentDrift);
}

static void test_great_circle_aliases_route() {
    // courseGreatCircle paths should populate the same fields as
    // courseRhumbline so either route style works.
    Data d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.courseGreatCircle.crossTrackError\",\"value\":12.0}"
                    "]}]}";
    sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.0, d.xte);
}

static void test_apstate_truncates_safely() {
    Data d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"steering.autopilot.state\","
                    "\"value\":\"a-very-long-state-name-that-overflows\"}"
                    "]}]}";
    sk::applyDelta(j, strlen(j), d);
    // sk::Data.apState is 16 bytes; expect a 15-char truncation + NUL.
    TEST_ASSERT_TRUE(strlen(d.apState) <= 15);
}

// classifyStatus: WS not connected -> "disconnected", regardless of timestamps.
static void test_status_disconnected() {
    TEST_ASSERT_EQUAL_STRING("disconnected",
                             sk::classifyStatus(false, 0, 0, 50000, 10000));
    TEST_ASSERT_EQUAL_STRING("disconnected",
                             sk::classifyStatus(false, 1000, 500, 50000, 10000));
}

// Regression: WS just connected, no delta yet, well inside the warmup
// window -> must be "live", not "stalled". Before the fix, lastUpdate==0
// alone forced "stalled" the instant set_connected(true) ran, so the
// "SIGNALK STALLED" alarm banner appeared on every fresh connect.
static void test_status_live_during_warmup() {
    // connectedSinceMs=1000, now=2000 -> 1s into warmup, no data yet.
    TEST_ASSERT_EQUAL_STRING("live",
                             sk::classifyStatus(true, 0, 1000, 2000, 10000));
}

// After the warmup window expires with still no data, surface as stalled.
static void test_status_stalled_after_warmup_no_data() {
    // connectedSinceMs=1000, now=12001 -> 11001ms since connect, > 10000.
    TEST_ASSERT_EQUAL_STRING("stalled",
                             sk::classifyStatus(true, 0, 1000, 12001, 10000));
}

// Fresh data within the stall window -> live.
static void test_status_live_with_fresh_data() {
    TEST_ASSERT_EQUAL_STRING(
        "live", sk::classifyStatus(true, 50000, 1000, 51000, 10000));
}

// Data went stale (last delta > stallMs ago) -> stalled.
static void test_status_stalled_with_stale_data() {
    TEST_ASSERT_EQUAL_STRING(
        "stalled", sk::classifyStatus(true, 1000, 500, 12000, 10000));
}

// Regression: a brief WS reconnect must restart the warmup window. The
// old behavior left lastUpdateMs at a value from before the disconnect,
// so `millis() - lastUpdateMs` was always huge after reconnect and the
// alarm fired immediately. signalk.cpp now resets lastUpdateMs to 0 on
// every connect; the classifier must treat that state as warmup, not
// stalled.
static void test_status_reconnect_resets_warmup() {
    // After reconnect: lastUpdateMs=0 (cleared by set_connected),
    // connectedSinceMs=200000, now=200500 -> 500ms into the new warmup.
    TEST_ASSERT_EQUAL_STRING(
        "live", sk::classifyStatus(true, 0, 200000, 200500, 10000));
}

// millis() wrap (~49 days uptime): nowMs wraps past lastUpdateMs but the
// real elapsed time is small. uint32_t subtraction must still give the
// correct ago.
static void test_status_handles_millis_wrap() {
    uint32_t last = 0xFFFFF000u;  // just before wrap
    uint32_t now = 0x00000100u;   // wrapped; ~4.3s after last
    TEST_ASSERT_EQUAL_STRING("live",
                             sk::classifyStatus(true, last, last, now, 10000));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_speed_over_ground);
    RUN_TEST(test_parses_apparent_wind);
    RUN_TEST(test_parses_position_object);
    RUN_TEST(test_parses_depth_variants);
    RUN_TEST(test_parses_battery_with_named_bank);
    RUN_TEST(test_parses_tanks);
    RUN_TEST(test_unknown_path_is_ignored);
    RUN_TEST(test_malformed_json_returns_error);
    RUN_TEST(test_keepalive_returns_zero);
    RUN_TEST(test_multiple_values_in_one_delta);
    RUN_TEST(test_null_value_does_not_overwrite);
    RUN_TEST(test_parses_route_fields);
    RUN_TEST(test_parses_autopilot_state_and_target);
    RUN_TEST(test_parses_current);
    RUN_TEST(test_great_circle_aliases_route);
    RUN_TEST(test_apstate_truncates_safely);
    RUN_TEST(test_status_disconnected);
    RUN_TEST(test_status_live_during_warmup);
    RUN_TEST(test_status_stalled_after_warmup_no_data);
    RUN_TEST(test_status_live_with_fresh_data);
    RUN_TEST(test_status_stalled_with_stale_data);
    RUN_TEST(test_status_reconnect_resets_warmup);
    RUN_TEST(test_status_handles_millis_wrap);
    return UNITY_END();
}
