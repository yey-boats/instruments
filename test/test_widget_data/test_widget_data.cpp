#include <unity.h>
#include <cmath>
#include <cstring>

#include "widget_data_resolver.h"

void setUp(void) {}
void tearDown(void) {}

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
    TEST_ASSERT_EQUAL_DOUBLE(3.5, widget_data::resolve_numeric(
        "navigation.speedOverGround", d));
    TEST_ASSERT_EQUAL_DOUBLE(1.60, widget_data::resolve_numeric(
        "navigation.headingTrue", d));
    TEST_ASSERT_EQUAL_DOUBLE(5.2, widget_data::resolve_numeric(
        "environment.wind.speedApparent", d));
    TEST_ASSERT_EQUAL_DOUBLE(0.82, widget_data::resolve_numeric(
        "electrical.batteries.house.stateOfCharge", d));
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
    TEST_ASSERT_TRUE(widget_data::resolve_string("boat.autopilotState", d,
                                                  buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("auto", buf);
    buf[0] = 'X'; buf[1] = 0;
    TEST_ASSERT_TRUE(widget_data::resolve_string("steering.autopilot.state", d,
                                                  buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("auto", buf);
}

static void test_string_path_unknown_returns_empty() {
    sk::Data d = make_data();
    char buf[16] = {'X', 0};
    TEST_ASSERT_FALSE(widget_data::resolve_string("boat.sog", d,
                                                   buf, sizeof(buf)));
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

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_local_alias_numeric);
    RUN_TEST(test_raw_sk_path_numeric);
    RUN_TEST(test_unknown_path_returns_nan);
    RUN_TEST(test_string_path_autopilot_state);
    RUN_TEST(test_string_path_unknown_returns_empty);
    RUN_TEST(test_is_known);
    return UNITY_END();
}
