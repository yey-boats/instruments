#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "midl_render.h"

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

static MetricBinding mapOne(const char *json, const char *id) {
    JsonDocument doc;
    deserializeJson(doc, json);
    static char idb[32], lab[32], unit[32];
    MetricBinding mb{};
    midl::render::map_element(doc.as<JsonVariantConst>(), id, mb, idb, lab, unit);
    return mb;
}

void test_token_to_kind() {
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, midl::render::token_to_kind("single-value"));
    TEST_ASSERT_EQUAL(WidgetKind::WindRose, midl::render::token_to_kind("windrose"));
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
    return UNITY_END();
}
