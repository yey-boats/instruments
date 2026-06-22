#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "midl_render.h"

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

// NOTE: mapOne returns a MetricBinding whose id/label/unit pointers refer to
// function-static buffers (idb/lab/unit). Do NOT call mapOne twice and compare
// both results — the second call overwrites those buffers, invalidating the first
// MetricBinding's string pointers.
static MetricBinding mapOne(const char *json, const char *id) {
    JsonDocument doc;
    deserializeJson(doc, json);
    static char idb[32], lab[32], unit[32], act[32];
    MetricBinding mb{};
    midl::render::map_element(doc.as<JsonVariantConst>(), id, mb, idb, lab, unit, act);
    return mb;
}

void test_token_to_kind() {
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, midl::render::token_to_kind("single-value"));
    TEST_ASSERT_EQUAL(WidgetKind::WindRose, midl::render::token_to_kind("windrose"));
    TEST_ASSERT_EQUAL(WidgetKind::WindSteer, midl::render::token_to_kind("windsteer"));
    TEST_ASSERT_EQUAL(WidgetKind::Compass, midl::render::token_to_kind("compass"));
    TEST_ASSERT_EQUAL(WidgetKind::Gauge, midl::render::token_to_kind("gauge"));
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, midl::render::token_to_kind("frobnicate"));
}

void test_map_single_value() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, m.kind);
    TEST_ASSERT_EQUAL_STRING("SOG", m.label);
    TEST_ASSERT_EQUAL_STRING("kn", m.unit);
    TEST_ASSERT_EQUAL_STRING("sog", m.id);
    TEST_ASSERT_EQUAL(MetricSource::SOG_kn, m.source);
}

void test_map_unknown_path_is_none() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"made.up.path"}}})",
        "x");
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);  // renders "--"
}

void test_map_compass_label_fallback_to_id() {
    MetricBinding m = mapOne(
        R"({"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(WidgetKind::Compass, m.kind);
    TEST_ASSERT_EQUAL_STRING("hdg", m.label);  // no name -> label falls back to id
}

void test_accent_hex_string() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","style":{"color":"#1a2b3c"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_color");
    TEST_ASSERT_EQUAL_HEX32(0x1A2B3Cu, m.accent);
}

void test_accent_integer() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","style":{"color":255},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_intcolor");
    TEST_ASSERT_EQUAL_HEX32(255u, m.accent);
}

// A gauge bound to the rudder with an explicit format.range + precision should
// carry both through to the painter binding.
void test_format_range_and_precision() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","name":"RUDDER","format":{"range":[-35,35],"precision":1,"unit":"deg"},
            "bindings":{"value":{"kind":"signalk","path":"steering.rudderAngle"}}})",
        "rudder");
    TEST_ASSERT_EQUAL(WidgetKind::Gauge, m.kind);
    TEST_ASSERT_EQUAL(MetricSource::Rudder_deg, m.source);
    TEST_ASSERT_EQUAL_FLOAT(-35.0f, m.range_min);
    TEST_ASSERT_EQUAL_FLOAT(35.0f, m.range_max);
    TEST_ASSERT_EQUAL_INT8(1, m.precision);
    TEST_ASSERT_EQUAL_STRING("deg", m.unit);
}

// Absent format.range/precision must leave the additive fields at their defaults
// (range_min==range_max==0, precision==-1) so legacy painter behavior is preserved.
void test_format_absent_defaults() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_gauge");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.range_min);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.range_max);
    TEST_ASSERT_EQUAL_INT8(-1, m.precision);
}

void test_action_nav_sets_target_screen() {
    MetricBinding m = mapOne(
        R"({"type":"button","name":"NAV","action":{"kind":"nav","target":"steering"}})", "go");
    TEST_ASSERT_EQUAL(WidgetKind::Button, m.kind);
    TEST_ASSERT_NOT_NULL(m.target_screen);
    TEST_ASSERT_EQUAL_STRING("steering", m.target_screen);
    TEST_ASSERT_NULL(m.command);
}

void test_action_command_sets_command() {
    MetricBinding m = mapOne(
        R"({"type":"button","name":"TACK","action":{"kind":"command","target":"tack"}})", "tk");
    TEST_ASSERT_EQUAL(WidgetKind::Button, m.kind);
    TEST_ASSERT_NOT_NULL(m.command);
    TEST_ASSERT_EQUAL_STRING("tack", m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void test_action_absent_leaves_both_null() {
    MetricBinding m = mapOne(R"({"type":"button","name":"PLAIN"})", "p");
    TEST_ASSERT_NULL(m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void test_action_unknown_kind_ignored() {
    MetricBinding m =
        mapOne(R"({"type":"button","action":{"kind":"frobnicate","target":"whatever"}})", "x");
    TEST_ASSERT_NULL(m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void setUp() {
}
void tearDown() {
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_token_to_kind);
    RUN_TEST(test_map_single_value);
    RUN_TEST(test_map_unknown_path_is_none);
    RUN_TEST(test_map_compass_label_fallback_to_id);
    RUN_TEST(test_accent_hex_string);
    RUN_TEST(test_accent_integer);
    RUN_TEST(test_format_range_and_precision);
    RUN_TEST(test_format_absent_defaults);
    RUN_TEST(test_action_nav_sets_target_screen);
    RUN_TEST(test_action_command_sets_command);
    RUN_TEST(test_action_absent_leaves_both_null);
    RUN_TEST(test_action_unknown_kind_ignored);
    return UNITY_END();
}
