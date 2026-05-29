// Host tests for the sources_check::from_string mapper used by
// manager apply_config to consume the cfg["sources"]["priority"] /
// ["timeoutsMs"] keys per spec 17 §6.

#include <unity.h>

#include "sources_check.h"
#include "boat_data.h"

using boat::SourceKind;
using sources_check::from_string;

void setUp(void) {}
void tearDown(void) {}

static void test_plugin_canonical_names_accepted() {
    // These are the strings the SignalK plugin uses in its default
    // profile (see signalk/config/plugin-config-data/...) - must map
    // 1:1.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::SignalK),
                          static_cast<int>(from_string("signalk")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::NmeaWifi),
                          static_cast<int>(from_string("nmea0183Wifi")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::Nmea2000),
                          static_cast<int>(from_string("nmea2000")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::Demo),
                          static_cast<int>(from_string("demo")));
}

static void test_legacy_source_name_output_accepted() {
    // boat::source_name(NmeaWifi) returns "nmea-wifi"; if a plugin or
    // operator round-trips through that, the input must still parse.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::NmeaWifi),
                          static_cast<int>(from_string("nmea-wifi")));
    // camelCase intermediate form also accepted.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::NmeaWifi),
                          static_cast<int>(from_string("nmeaWifi")));
}

static void test_unknown_returns_none() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string("usb")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string("Demo")));   // case-sensitive
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string("SignalK"))); // case-sensitive
}

static void test_null_and_empty_return_none() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string(nullptr)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string("")));
}

static void test_explicit_none_maps_to_none() {
    // Sometimes a config explicitly clears a slot by writing "none";
    // mapping must accept it as a real value rather than treating it
    // as "unknown error".
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SourceKind::None),
                          static_cast<int>(from_string("none")));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_plugin_canonical_names_accepted);
    RUN_TEST(test_legacy_source_name_output_accepted);
    RUN_TEST(test_unknown_returns_none);
    RUN_TEST(test_null_and_empty_return_none);
    RUN_TEST(test_explicit_none_maps_to_none);
    return UNITY_END();
}
