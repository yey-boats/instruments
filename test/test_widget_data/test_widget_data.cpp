#include <unity.h>
#include <cmath>
#include <cstring>

#include "widget_data_resolver.h"

void setUp(void) {
}
void tearDown(void) {
}

static sk::Data make_data() {
    sk::Data d;
    d.sog = 3.5;
    d.cogTrue = 1.57;
    d.headingTrue = 1.60;
    d.depth = 12.3;
    d.battVoltage = 12.7;
    d.battSoc = 0.82;
    d.awa = -0.5;
    d.aws = 5.2;
    strncpy(d.apState, "auto", sizeof(d.apState) - 1);
    d.apTargetHdg = 1.5;
    return d;
}

static void test_local_alias_numeric() {
    sk::Data d = make_data();
    TEST_ASSERT_EQUAL_DOUBLE(3.5, widget_data::resolve_numeric("boat.sog", d));
    TEST_ASSERT_EQUAL_DOUBLE(12.3, widget_data::resolve_numeric("boat.depth", d));
    TEST_ASSERT_EQUAL_DOUBLE(12.7, widget_data::resolve_numeric("boat.batteryVoltage", d));
    TEST_ASSERT_EQUAL_DOUBLE(-0.5, widget_data::resolve_numeric("boat.awa", d));
}

static void test_raw_sk_path_numeric() {
    sk::Data d = make_data();
    TEST_ASSERT_EQUAL_DOUBLE(3.5, widget_data::resolve_numeric("navigation.speedOverGround", d));
    TEST_ASSERT_EQUAL_DOUBLE(1.60, widget_data::resolve_numeric("navigation.headingTrue", d));
    TEST_ASSERT_EQUAL_DOUBLE(5.2,
                             widget_data::resolve_numeric("environment.wind.speedApparent", d));
    TEST_ASSERT_EQUAL_DOUBLE(
        0.82, widget_data::resolve_numeric("electrical.batteries.house.stateOfCharge", d));
}

static void test_unknown_path_returns_nan() {
    sk::Data d = make_data();
    double v = widget_data::resolve_numeric("not.a.path", d);
    TEST_ASSERT_TRUE(std::isnan(v));
    TEST_ASSERT_TRUE(std::isnan(widget_data::resolve_numeric(nullptr, d)));
    TEST_ASSERT_TRUE(std::isnan(widget_data::resolve_numeric("", d)));
}

static void test_string_path_autopilot_state() {
    sk::Data d = make_data();
    char buf[24] = {0};
    TEST_ASSERT_TRUE(widget_data::resolve_string("boat.autopilotState", d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("auto", buf);
    buf[0] = 'X';
    buf[1] = 0;
    TEST_ASSERT_TRUE(widget_data::resolve_string("steering.autopilot.state", d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("auto", buf);
}

static void test_string_path_unknown_returns_empty() {
    sk::Data d = make_data();
    char buf[16] = {'X', 0};
    TEST_ASSERT_FALSE(widget_data::resolve_string("boat.sog", d, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_is_known() {
    TEST_ASSERT_TRUE(widget_data::is_known("boat.sog"));
    TEST_ASSERT_TRUE(widget_data::is_known("navigation.speedOverGround"));
    TEST_ASSERT_TRUE(widget_data::is_known("boat.autopilotState"));
    TEST_ASSERT_FALSE(widget_data::is_known("nope.path"));
    TEST_ASSERT_FALSE(widget_data::is_known(""));
    TEST_ASSERT_FALSE(widget_data::is_known(nullptr));
}

static void test_resolve_falls_back_to_store_for_unknown_path() {
    sk::Data d = make_data();
    sk::PathStore store;
    store.set("propulsion.0.revolutions", 27.5);
    // Unknown to the typed resolver -> uses the store.
    TEST_ASSERT_EQUAL_DOUBLE(27.5,
                             widget_data::resolve_numeric("propulsion.0.revolutions", d, &store));
}

static void test_known_path_prefers_typed_field_over_store() {
    sk::Data d = make_data();  // sog = 3.5
    sk::PathStore store;
    store.set("navigation.speedOverGround", 99.0);  // stale store value
    // Known typed field wins; store is only the fallback.
    TEST_ASSERT_EQUAL_DOUBLE(3.5,
                             widget_data::resolve_numeric("navigation.speedOverGround", d, &store));
}

static void test_unknown_path_without_store_is_nan() {
    sk::Data d = make_data();
    TEST_ASSERT_TRUE(std::isnan(widget_data::resolve_numeric("x.y.z", d, nullptr)));
}

static void test_capture_dynamic_round_trip() {
    sk::PathStore store;
    TEST_ASSERT_TRUE(widget_data::captureDynamic("foo.bar", 1.25, store));
    TEST_ASSERT_EQUAL_DOUBLE(1.25, store.get("foo.bar"));
}

static void test_capture_then_resolve_arbitrary_path() {
    sk::Data d = make_data();
    sk::PathStore store;
    // Simulate a WS delta for a path the typed parser does not know.
    widget_data::captureDynamic("electrical.solar.0.power", 142.0, store);
    // The renderer resolves the authored field by its path string.
    TEST_ASSERT_EQUAL_DOUBLE(142.0,
                             widget_data::resolve_numeric("electrical.solar.0.power", d, &store));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_local_alias_numeric);
    RUN_TEST(test_raw_sk_path_numeric);
    RUN_TEST(test_unknown_path_returns_nan);
    RUN_TEST(test_string_path_autopilot_state);
    RUN_TEST(test_string_path_unknown_returns_empty);
    RUN_TEST(test_is_known);
    RUN_TEST(test_resolve_falls_back_to_store_for_unknown_path);
    RUN_TEST(test_known_path_prefers_typed_field_over_store);
    RUN_TEST(test_unknown_path_without_store_is_nan);
    RUN_TEST(test_capture_dynamic_round_trip);
    RUN_TEST(test_capture_then_resolve_arbitrary_path);
    return UNITY_END();
}
