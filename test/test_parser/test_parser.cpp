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
    return UNITY_END();
}
