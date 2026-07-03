#include <unity.h>
#include <cmath>
#include <cstring>
#include <string>
#include "signalk_parser.h"

using boat::View;

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
    View d;
    auto j = singleValueDelta("navigation.speedOverGround", "3.5");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.5, d.sog);
}

static void test_parses_apparent_wind() {
    View d;
    auto j = singleValueDelta("environment.wind.angleApparent", "0.785");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.785, d.awa);

    auto j2 = singleValueDelta("environment.wind.speedApparent", "8.2");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 8.2, d.aws);
}

static void test_parses_polar_angles() {
    View d;
    auto j = singleValueDelta("performance.beatAngle", "0.7330");  // ~42 deg
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.7330, d.beatAngle);

    auto j2 = singleValueDelta("performance.gybeAngle", "2.7053");  // ~155 deg
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 2.7053, d.gybeAngle);
}

static void test_parses_position_object() {
    View d;
    auto j = singleValueDelta("navigation.position", "{\"latitude\":41.3851,\"longitude\":2.1734}");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);
    // ArduinoJson stores floats by default - 1e-4 deg ~= 11 m, enough for marine UI.
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 41.3851, d.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 2.1734, d.lon);
}

static void test_parses_depth_variants() {
    View d;
    auto j1 = singleValueDelta("environment.depth.belowTransducer", "12.3");
    sk::applyDelta(j1.data(), j1.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.3, d.depth);
    TEST_ASSERT_TRUE(std::isnan(d.depthKeel));  // belowKeel must not overwrite depth

    View d2;
    auto j2 = singleValueDelta("environment.depth.belowKeel", "8.5");
    sk::applyDelta(j2.data(), j2.size(), d2);
    // belowKeel must populate depthKeel, not depth
    TEST_ASSERT_FLOAT_WITHIN(0.001, 8.5, d2.depthKeel);
    TEST_ASSERT_TRUE(std::isnan(d2.depth));

    // Both fields independent when both paths arrive in the same delta
    View d3;
    sk::applyDelta(j1.data(), j1.size(), d3);  // sets depth
    sk::applyDelta(j2.data(), j2.size(), d3);  // sets depthKeel
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.3, d3.depth);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 8.5, d3.depthKeel);
}

static void test_parses_battery_with_named_bank() {
    View d;
    auto j = singleValueDelta("electrical.batteries.house.voltage", "12.7");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.7, d.battVoltage);

    auto j2 = singleValueDelta("electrical.batteries.house.stateOfCharge", "0.82");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.82, d.battSoc);
}

static void test_parses_tanks() {
    View d;
    auto j = singleValueDelta("tanks.fuel.0.currentLevel", "0.65");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.65, d.tankFuel);

    auto j2 = singleValueDelta("tanks.freshWater.starboard.currentLevel", "0.4");
    sk::applyDelta(j2.data(), j2.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.4, d.tankWater);
}

static void test_unknown_path_is_ignored() {
    View d;
    auto j = singleValueDelta("some.random.unknown.path", "42");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(1, n);              // the value was seen
    TEST_ASSERT_TRUE(std::isnan(d.sog));  // but no known field was set
    TEST_ASSERT_TRUE(std::isnan(d.depth));
}

static void test_malformed_json_returns_error() {
    View d;
    const char *bad = "{not valid json";
    int n = sk::applyDelta(bad, strlen(bad), d);
    TEST_ASSERT_LESS_THAN(0, n);
}

static void test_keepalive_returns_zero() {
    View d;
    // SignalK hello/keepalive frames don't contain an "updates" array.
    const char *hello = "{\"name\":\"signalk-server\",\"version\":\"2.27.0\"}";
    int n = sk::applyDelta(hello, strlen(hello), d);
    TEST_ASSERT_EQUAL(0, n);
}

static void test_multiple_values_in_one_delta() {
    View d;
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
    View d;
    d.sog = 5.0;  // pre-set
    auto j = singleValueDelta("navigation.speedOverGround", "null");
    sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 5.0, d.sog);  // unchanged
}

static void test_parses_route_fields() {
    View d;
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
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"steering.autopilot.target.headingTrue\",\"value\":1.234},"
                    "{\"path\":\"steering.autopilot.state\",\"value\":\"auto\"}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.234, d.apTargetHdg);
    TEST_ASSERT_EQUAL_STRING("auto", d.apState);
}

static void test_parses_rudder_angle() {
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"steering.rudderAngle\",\"value\":-0.2618}"  // -15 deg
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, -0.2618, d.rudder);
}

static void test_parses_vmg_performance_path() {
    // performance.velocityMadeGood is the WIND/polar VMG — a DISTINCT metric from
    // the waypoint VMG (navigation.courseRhumbline.velocityMadeGood). It must land
    // on vmgWind and NOT touch the route vmg field.
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"performance.velocityMadeGood\",\"value\":-1.83}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, -1.83, d.vmgWind);
    TEST_ASSERT_TRUE(isnan(d.vmg));  // route VMG untouched by the performance path
}

static void test_vmg_split_distinct_fields() {
    // The waypoint VMG and the wind VMG are two distinct readouts: the route
    // path -> vmg, the performance path -> vmgWind, in a single delta.
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.courseRhumbline.velocityMadeGood\",\"value\":4.2},"
                    "{\"path\":\"performance.velocityMadeGood\",\"value\":2.7}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 4.2, d.vmg);      // waypoint VMG
    TEST_ASSERT_FLOAT_WITHIN(0.001, 2.7, d.vmgWind);  // wind/polar VMG
}

static void test_parses_current() {
    View d;
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
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.courseGreatCircle.crossTrackError\",\"value\":12.0}"
                    "]}]}";
    sk::applyDelta(j, strlen(j), d);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.0, d.xte);
}

static void test_apstate_truncates_safely() {
    View d;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"steering.autopilot.state\","
                    "\"value\":\"a-very-long-state-name-that-overflows\"}"
                    "]}]}";
    sk::applyDelta(j, strlen(j), d);
    // boat::View.apState is 16 bytes; expect a 15-char truncation + NUL.
    TEST_ASSERT_TRUE(strlen(d.apState) <= 15);
}

// --- coverage wave: environment / attitude / engine / power paths ---

static void test_parses_outside_environment() {
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"environment.outside.temperature\",\"value\":291.15},"
                    "{\"path\":\"environment.outside.pressure\",\"value\":101300},"
                    "{\"path\":\"environment.outside.relativeHumidity\",\"value\":0.62}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 291.15, d.outsideTemp);
    TEST_ASSERT_FLOAT_WITHIN(0.5, 101300.0, d.outsidePressure);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.62, d.humidity);
    boat::FieldMask want = boat::field_bit(boat::FieldId::OutsideTemp) |
                           boat::field_bit(boat::FieldId::OutsidePressure) |
                           boat::field_bit(boat::FieldId::Humidity);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_parses_humidity_legacy_alias() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("environment.outside.humidity", "0.55");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.55, d.humidity);
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::Humidity), touched);
}

static void test_parses_attitude_object() {
    // {roll, pitch, yaw} object parsed like navigation.position; yaw ignored.
    View d;
    boat::FieldMask touched = 0;
    auto j =
        singleValueDelta("navigation.attitude", "{\"roll\":-0.0872,\"pitch\":0.0349,\"yaw\":1.57}");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -0.0872, d.roll);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.0349, d.pitch);
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::Roll) | boat::field_bit(boat::FieldId::Pitch);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_parses_attitude_partial_object() {
    // Only roll present -> only the Roll bit may be set.
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("navigation.attitude", "{\"roll\":0.1}");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.1, d.roll);
    TEST_ASSERT_TRUE(std::isnan(d.pitch));
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::Roll), touched);
}

static void test_parses_attitude_leaf_paths() {
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.attitude.roll\",\"value\":0.05},"
                    "{\"path\":\"navigation.attitude.pitch\",\"value\":-0.02}"
                    "]}]}";
    sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.05, d.roll);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -0.02, d.pitch);
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::Roll) | boat::field_bit(boat::FieldId::Pitch);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_parses_rate_of_turn() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("navigation.rateOfTurn", "0.0175");  // ~1 deg/s to stbd
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.0175, d.rateOfTurn);
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::RateOfTurn), touched);
}

static void test_parses_trip_and_total_log() {
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.trip.log\",\"value\":9260},"
                    "{\"path\":\"navigation.log\",\"value\":1852000}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.5, 9260.0, d.tripLog);      // 5 nm in metres
    TEST_ASSERT_FLOAT_WITHIN(0.5, 1852000.0, d.totalLog);  // 1000 nm
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::TripLog) | boat::field_bit(boat::FieldId::TotalLog);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_parses_battery_current_and_temperature() {
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"electrical.batteries.house.current\",\"value\":-12.4},"
                    "{\"path\":\"electrical.batteries.house.temperature\",\"value\":298.15}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, -12.4, d.battCurrent);  // signed: discharging
    TEST_ASSERT_FLOAT_WITHIN(0.001, 298.15, d.battTemp);
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::BattCurrent) | boat::field_bit(boat::FieldId::BattTemp);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_parses_propulsion_any_instance() {
    // Instance segment is prefix-matched like electrical.batteries.* — the
    // first/primary engine wins the typed fields whatever it is named.
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"propulsion.main.revolutions\",\"value\":30},"
                    "{\"path\":\"propulsion.main.temperature\",\"value\":361.15},"
                    "{\"path\":\"propulsion.main.oilPressure\",\"value\":350000},"
                    "{\"path\":\"propulsion.main.fuel.rate\",\"value\":0.0000012}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 30.0, d.engineRevs);  // Hz (=1800 RPM)
    TEST_ASSERT_FLOAT_WITHIN(0.001, 361.15, d.engineCoolantTemp);
    TEST_ASSERT_FLOAT_WITHIN(1.0, 350000.0, d.engineOilPressure);  // 3.5 bar
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0000012, d.engineFuelRate);
    boat::FieldMask want = boat::field_bit(boat::FieldId::EngineRpm) |
                           boat::field_bit(boat::FieldId::EngineCoolantTemp) |
                           boat::field_bit(boat::FieldId::EngineOilPressure) |
                           boat::field_bit(boat::FieldId::EngineFuelRate);
    TEST_ASSERT_EQUAL_UINT64(want, touched);

    // A differently-named instance still lands on the typed fields.
    View d2;
    auto j2 = singleValueDelta("propulsion.port.revolutions", "25");
    sk::applyDelta(j2.data(), j2.size(), d2);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 25.0, d2.engineRevs);
}

static void test_propulsion_unrelated_leaf_ignored() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("propulsion.main.oilTemperature", "320");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    // oilTemperature must NOT collide with the ".temperature" coolant suffix.
    TEST_ASSERT_TRUE(std::isnan(d.engineCoolantTemp));
    TEST_ASSERT_EQUAL_UINT64(0, touched);
}

static void test_parses_heading_magnetic_and_variation() {
    // BUG-2: magnetic heading and variation land on their OWN fields and must
    // NOT touch headingTrue.
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"updates\":[{\"values\":["
                    "{\"path\":\"navigation.headingMagnetic\",\"value\":1.0},"
                    "{\"path\":\"navigation.magneticVariation\",\"value\":0.05}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 1.0, d.headingMag);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.05, d.variation);
    TEST_ASSERT_TRUE(std::isnan(d.headingTrue));  // not conflated
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::HeadingMag) | boat::field_bit(boat::FieldId::Variation);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

// --- touched-mask (per-delta field tracking for the staleness-honest ingest) ---

static void test_touched_mask_single_field() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("navigation.speedOverGround", "3.5");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(1, n);
    // Exactly the SOG bit - nothing else in this delta.
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::Sog), touched);
}

static void test_touched_mask_multiple_fields_accumulate() {
    View d;
    boat::FieldMask touched = 0;
    const char *j = "{\"context\":\"vessels.self\",\"updates\":[{"
                    "\"values\":["
                    "{\"path\":\"navigation.speedOverGround\",\"value\":4.0},"
                    "{\"path\":\"environment.depth.belowTransducer\",\"value\":7.5}"
                    "]}]}";
    int n = sk::applyDelta(j, strlen(j), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(2, n);
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::Sog) | boat::field_bit(boat::FieldId::Depth);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_touched_mask_survives_stale_accumulator() {
    // The device parses into a persistent accumulator View: a field set by a
    // PREVIOUS delta stays non-NaN, but a new delta that doesn't mention it
    // must NOT mark it touched (that would re-stamp a dead sensor fresh).
    View d;
    auto j1 = singleValueDelta("environment.depth.belowTransducer", "7.5");
    sk::applyDelta(j1.data(), j1.size(), d);  // accumulator now holds depth
    boat::FieldMask touched = 0;
    auto j2 = singleValueDelta("navigation.speedOverGround", "4.0");
    sk::applyDelta(j2.data(), j2.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 7.5, d.depth);  // still in the accumulator
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::Sog), touched);  // but NOT touched
}

static void test_touched_mask_null_value_not_marked() {
    View d;
    d.sog = 5.0;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("navigation.speedOverGround", "null");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 5.0, d.sog);  // unchanged...
    TEST_ASSERT_EQUAL_UINT64(0, touched);         // ...and not marked touched
}

static void test_touched_mask_unknown_path_not_marked() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("some.random.unknown.path", "42");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(1, n);               // seen (dyn-store eligible)...
    TEST_ASSERT_EQUAL_UINT64(0, touched);  // ...but no typed field touched
}

static void test_touched_mask_position_sets_lat_lon() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("navigation.position", "{\"latitude\":41.3,\"longitude\":2.17}");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    boat::FieldMask want =
        boat::field_bit(boat::FieldId::Lat) | boat::field_bit(boat::FieldId::Lon);
    TEST_ASSERT_EQUAL_UINT64(want, touched);
}

static void test_touched_mask_apstate() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("steering.autopilot.state", "\"auto\"");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL_STRING("auto", d.apState);
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::ApState), touched);
}

// --- Context routing (phase 5): self vs vessels.* (AIS) + notifications ----

// Build a delta with an explicit context.
static std::string contextDelta(const char *ctx, const char *path, const char *valueJson) {
    std::string s = "{\"context\":\"";
    s += ctx;
    s += "\",\"updates\":[{\"values\":[{\"path\":\"";
    s += path;
    s += "\",\"value\":";
    s += valueJson;
    s += "}]}]}";
    return s;
}

static const char *SELF_ID = "vessels.urn:mrn:imo:mmsi:239000001";

static void test_context_other_vessel_routes_to_ais_not_view() {
    View d;
    ais::Store st;
    boat::FieldMask touched = 0;
    sk::DeltaSinks sinks;
    sinks.ais = &st;
    sinks.self_id = SELF_ID;
    sinks.now_ms = 5000;
    auto j = contextDelta("vessels.urn:mrn:imo:mmsi:244813000", "navigation.position",
                          "{\"latitude\":41.31,\"longitude\":2.18}");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched, &sinks);
    TEST_ASSERT_EQUAL(1, n);  // routed values count for link liveness
    TEST_ASSERT_TRUE(std::isnan(d.lat));
    TEST_ASSERT_TRUE(std::isnan(d.lon));
    TEST_ASSERT_EQUAL_UINT64(0, touched);  // never touches the self mask
    TEST_ASSERT_EQUAL(1, st.count());
    ais::Target t;
    TEST_ASSERT_TRUE(st.get(st.find(244813000u), t));
    // 1e-4 deg (~10 m): ArduinoJson may store the literal at float precision.
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 41.31, t.lat_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 2.18, t.lon_deg);
    TEST_ASSERT_EQUAL_UINT32(5000, t.last_seen_ms);
}

static void test_context_other_vessel_sog_cog_merge() {
    View d;
    ais::Store st;
    sk::DeltaSinks sinks;
    sinks.ais = &st;
    sinks.self_id = SELF_ID;
    sinks.now_ms = 1000;
    const char *ctx = "vessels.urn:mrn:imo:mmsi:244813000";
    auto j1 = contextDelta(ctx, "navigation.speedOverGround", "6.2");
    auto j2 = contextDelta(ctx, "navigation.courseOverGroundTrue", "1.5");
    sk::applyDelta(j1.data(), j1.size(), d, nullptr, nullptr, nullptr, &sinks);
    sk::applyDelta(j2.data(), j2.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(1, st.count());  // dedup by MMSI across deltas
    ais::Target t;
    TEST_ASSERT_TRUE(st.get(0, t));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 6.2f, t.sog_mps);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.5f, t.cog_rad);
    TEST_ASSERT_TRUE(std::isnan(d.sog));  // self View untouched
    TEST_ASSERT_TRUE(std::isnan(d.cogTrue));
}

static void test_context_other_vessel_name_from_static_tree() {
    View d;
    ais::Store st;
    sk::DeltaSinks sinks;
    sinks.ais = &st;
    sinks.self_id = SELF_ID;
    sinks.now_ms = 1000;
    const char *ctx = "vessels.urn:mrn:imo:mmsi:244813000";
    auto j1 = contextDelta(ctx, "name", "\"NORDKAPP\"");
    auto j2 = contextDelta(ctx, "", "{\"name\":\"SOLVEIG\"}");  // root-object form
    sk::applyDelta(j1.data(), j1.size(), d, nullptr, nullptr, nullptr, &sinks);
    ais::Target t;
    TEST_ASSERT_TRUE(st.get(st.find(244813000u), t));
    TEST_ASSERT_EQUAL_STRING("NORDKAPP", t.name);
    sk::applyDelta(j2.data(), j2.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_TRUE(st.get(st.find(244813000u), t));
    TEST_ASSERT_EQUAL_STRING("SOLVEIG", t.name);
    TEST_ASSERT_EQUAL(1, st.count());
}

static void test_context_self_urn_still_fills_view() {
    // Deltas about OUR OWN ship carry the full urn context; with self_id
    // from the hello they must keep landing on the View, not the AIS store.
    View d;
    ais::Store st;
    boat::FieldMask touched = 0;
    sk::DeltaSinks sinks;
    sinks.ais = &st;
    sinks.self_id = SELF_ID;
    sinks.now_ms = 1000;
    auto j =
        contextDelta("vessels.urn:mrn:imo:mmsi:239000001", "navigation.speedOverGround", "4.2");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched, &sinks);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 4.2, d.sog);
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::Sog), touched);
    TEST_ASSERT_EQUAL(0, st.count());
}

static void test_context_self_id_matches_without_vessels_prefix() {
    // Some servers report hello self WITHOUT the "vessels." prefix.
    View d;
    sk::DeltaSinks sinks;
    sinks.self_id = "urn:mrn:imo:mmsi:239000001";
    sinks.now_ms = 1000;
    auto j =
        contextDelta("vessels.urn:mrn:imo:mmsi:239000001", "navigation.speedOverGround", "4.2");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 4.2, d.sog);
}

static void test_context_uuid_vessel_dropped() {
    // Non-MMSI vessels (uuid urns) can't key the AIS store - dropped whole.
    View d;
    ais::Store st;
    sk::DeltaSinks sinks;
    sinks.ais = &st;
    sinks.self_id = SELF_ID;
    sinks.now_ms = 1000;
    auto j =
        contextDelta("vessels.urn:mrn:signalk:uuid:aaaa-bbbb", "navigation.speedOverGround", "6.0");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_EQUAL(0, st.count());
    TEST_ASSERT_TRUE(std::isnan(d.sog));
}

static void test_context_other_vessel_without_sinks_ignored() {
    // Legacy callers (no sinks): a non-self delta is dropped, not misapplied.
    View d;
    auto j =
        contextDelta("vessels.urn:mrn:imo:mmsi:244813000", "navigation.speedOverGround", "6.0");
    int n = sk::applyDelta(j.data(), j.size(), d);
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_TRUE(std::isnan(d.sog));
}

static void test_notification_upsert_from_delta() {
    View d;
    notif::Store st;
    sk::DeltaSinks sinks;
    sinks.notifications = &st;
    sinks.now_ms = 7000;
    auto j = singleValueDelta("notifications.navigation.anchor",
                              "{\"state\":\"emergency\",\"message\":\"Anchor drag!\","
                              "\"method\":[\"visual\",\"sound\"]}");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(1, st.count());
    notif::Entry e;
    TEST_ASSERT_TRUE(st.get(0, e));
    TEST_ASSERT_EQUAL_STRING("navigation.anchor", e.path);
    TEST_ASSERT_EQUAL_STRING("Anchor drag!", e.message);
    TEST_ASSERT_EQUAL(static_cast<int>(notif::State::Emergency), static_cast<int>(e.state));
    TEST_ASSERT_EQUAL_UINT8(notif::METHOD_VISUAL | notif::METHOD_SOUND, e.method);
    TEST_ASSERT_EQUAL_UINT32(7000, e.first_ms);
}

static void test_notification_normal_state_clears() {
    View d;
    notif::Store st;
    sk::DeltaSinks sinks;
    sinks.notifications = &st;
    sinks.now_ms = 7000;
    auto raise = singleValueDelta("notifications.environment.depth",
                                  "{\"state\":\"alarm\",\"message\":\"Shallow\"}");
    sk::applyDelta(raise.data(), raise.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(1, st.count());
    auto clear = singleValueDelta("notifications.environment.depth",
                                  "{\"state\":\"normal\",\"message\":\"ok\"}");
    sk::applyDelta(clear.data(), clear.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(0, st.count());
    // Null value clears too (alarm withdrawn entirely).
    sk::applyDelta(raise.data(), raise.size(), d, nullptr, nullptr, nullptr, &sinks);
    auto nulled = singleValueDelta("notifications.environment.depth", "null");
    sk::applyDelta(nulled.data(), nulled.size(), d, nullptr, nullptr, nullptr, &sinks);
    TEST_ASSERT_EQUAL(0, st.count());
}

static void test_notification_path_never_touches_view_mask() {
    View d;
    notif::Store st;
    boat::FieldMask touched = 0;
    sk::DeltaSinks sinks;
    sinks.notifications = &st;
    sinks.now_ms = 100;
    auto j = singleValueDelta("notifications.navigation.anchor", "{\"state\":\"alert\"}");
    sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched, &sinks);
    TEST_ASSERT_EQUAL_UINT64(0, touched);
}

static void test_parse_hello_extracts_self() {
    const char *hello = "{\"name\":\"sk\",\"version\":\"2.0.0\","
                        "\"self\":\"vessels.urn:mrn:imo:mmsi:239000001\",\"roles\":[]}";
    char out[96] = {0};
    TEST_ASSERT_TRUE(sk::parseHello(hello, strlen(hello), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("vessels.urn:mrn:imo:mmsi:239000001", out);
    const char *delta = "{\"updates\":[]}";
    TEST_ASSERT_FALSE(sk::parseHello(delta, strlen(delta), out, sizeof(out)));
}

static void test_parses_engine_run_time() {
    View d;
    boat::FieldMask touched = 0;
    auto j = singleValueDelta("propulsion.main.runTime", "9000.0");
    int n = sk::applyDelta(j.data(), j.size(), d, nullptr, nullptr, &touched);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 9000.0, d.engineHours);  // seconds, SI
    TEST_ASSERT_EQUAL_UINT64(boat::field_bit(boat::FieldId::EngineHours), touched);
}

// classifyStatus: WS not connected -> "disconnected", regardless of timestamps.
static void test_status_disconnected() {
    TEST_ASSERT_EQUAL_STRING("disconnected", sk::classifyStatus(false, 0, 0, 0, 50000, 10000));
    TEST_ASSERT_EQUAL_STRING("disconnected",
                             sk::classifyStatus(false, 1000, 500, 1000, 50000, 10000));
}

// Regression: WS just connected, no delta yet, well inside the warmup
// window -> must be "live", not "stalled". Before the fix, lastUpdate==0
// alone forced "stalled" the instant set_connected(true) ran, so the
// "SIGNALK STALLED" alarm banner appeared on every fresh connect.
static void test_status_live_during_warmup() {
    // connectedSinceMs=1000, now=2000 -> 1s into warmup, no data, no frames yet.
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, 0, 1000, 0, 2000, 10000));
}

// After the warmup window expires with still no data and no frames, stalled.
static void test_status_stalled_after_warmup_no_activity() {
    // connectedSinceMs=1000, now=12001 -> 11001ms since connect, > 10000.
    TEST_ASSERT_EQUAL_STRING("stalled", sk::classifyStatus(true, 0, 1000, 0, 12001, 10000));
}

// Fresh data within the stall window -> live.
static void test_status_live_with_fresh_data() {
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, 50000, 1000, 50000, 51000, 10000));
}

// View went stale (last delta > stallMs ago) and no other activity -> stalled.
static void test_status_stalled_with_stale_data() {
    TEST_ASSERT_EQUAL_STRING("stalled", sk::classifyStatus(true, 1000, 500, 1000, 12000, 10000));
}

// Regression: a brief WS reconnect must restart the warmup window. The
// old behavior left lastUpdateMs at a value from before the disconnect,
// so `millis() - lastUpdateMs` was always huge after reconnect and the
// alarm fired immediately. signalk.cpp now resets lastUpdateMs and
// wsLastFrameMs to 0 on every connect; the classifier must treat that
// state as warmup, not stalled.
static void test_status_reconnect_resets_warmup() {
    // After reconnect: lastUpdateMs=0, wsLastFrameMs=0,
    // connectedSinceMs=200000, now=200500 -> 500ms into the new warmup.
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, 0, 200000, 0, 200500, 10000));
}

// millis() wrap (~49 days uptime): nowMs wraps past lastUpdateMs but the
// real elapsed time is small. uint32_t subtraction must still give the
// correct ago.
static void test_status_handles_millis_wrap() {
    uint32_t last = 0xFFFFF000u;  // just before wrap
    uint32_t now = 0x00000100u;   // wrapped; ~4.3s after last
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, last, last, last, now, 10000));
}

// WS link activity (hello, subscription ack, envelope-only delta)
// happened within the window but no value-bearing delta did, and we're
// still inside the no-data grace window. Holds the status at "live".
static void test_status_live_when_frames_arrive_without_values() {
    // connectedSinceMs=45000, wsLastFrame=49000, now=50000 -> 5s connected,
    // last frame 1s ago. stall=10s, no-data grace=60s (long).
    TEST_ASSERT_EQUAL_STRING("live",
                             sk::classifyStatus(true, 0, 45000, 49000, 50000, 10000, 60000));
}

// New: WS connected and frames flowing, but no value-bearing delta has
// landed AND we've been connected longer than the no-data grace window.
// This is the "server has no producers" case the user hit.
static void test_status_no_data_after_grace() {
    // connectedSinceMs=1000, wsLastFrame=49000, lastUpdate=0, now=50000,
    // stall=30s (frames are 1s old -> not stalled), no-data grace=10s.
    // Connected 49s with no values -> "no-data".
    TEST_ASSERT_EQUAL_STRING("no-data",
                             sk::classifyStatus(true, 0, 1000, 49000, 50000, 30000, 10000));
}

// no-data only fires after the grace window; before that we're "live".
static void test_status_live_within_no_data_grace() {
    // connectedSinceMs=45000, wsLastFrame=49000, now=50000 -> 5s connected.
    // Grace=10s -> still "live".
    TEST_ASSERT_EQUAL_STRING("live",
                             sk::classifyStatus(true, 0, 45000, 49000, 50000, 30000, 10000));
}

// stalled wins over no-data: if the WS link itself goes silent, that's
// the higher-severity state.
static void test_status_stalled_beats_no_data() {
    // No frames for 40s, no values ever, connected since 1000, now=50000,
    // stall=30s -> "stalled", not "no-data".
    TEST_ASSERT_EQUAL_STRING("stalled",
                             sk::classifyStatus(true, 0, 1000, 1000, 50000, 30000, 10000));
}

// New: no WS activity of any kind within the window -> stalled even if
// connectedSinceMs is recent (which it isn't here; warmup already passed).
static void test_status_stalled_when_all_signals_stale() {
    // All three timestamps are 20s old, window is 10s -> stalled.
    TEST_ASSERT_EQUAL_STRING("stalled", sk::classifyStatus(true, 1000, 1000, 1000, 21000, 10000));
}

// New: frames arrive but values don't. Frames are more recent than the
// last value-bearing delta. The frame timestamp wins as the staleness ref.
static void test_status_frames_more_recent_than_values() {
    // lastUpdateMs=1000 (old), wsLastFrameMs=49000 (recent), now=50000.
    // Stall window 10000 -> ref=49000, ago=1000 -> live.
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, 1000, 500, 49000, 50000, 10000));
}

// New: default-arg threshold (30s) is what production uses. Verify a
// 25s-old delta is "live" at the default, while at the legacy 10s
// threshold the same state would have been "stalled".
static void test_status_default_threshold_is_30s() {
    // ref=25000, now=50000 -> 25s ago.
    TEST_ASSERT_EQUAL_STRING("live", sk::classifyStatus(true, 25000, 25000, 25000, 50000));
    // At the legacy 10s threshold, same state would alarm.
    TEST_ASSERT_EQUAL_STRING("stalled",
                             sk::classifyStatus(true, 25000, 25000, 25000, 50000, 10000));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_speed_over_ground);
    RUN_TEST(test_parses_apparent_wind);
    RUN_TEST(test_parses_polar_angles);
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
    RUN_TEST(test_parses_rudder_angle);
    RUN_TEST(test_parses_vmg_performance_path);
    RUN_TEST(test_vmg_split_distinct_fields);
    RUN_TEST(test_parses_current);
    RUN_TEST(test_great_circle_aliases_route);
    RUN_TEST(test_apstate_truncates_safely);
    RUN_TEST(test_parses_outside_environment);
    RUN_TEST(test_parses_humidity_legacy_alias);
    RUN_TEST(test_parses_attitude_object);
    RUN_TEST(test_parses_attitude_partial_object);
    RUN_TEST(test_parses_attitude_leaf_paths);
    RUN_TEST(test_parses_rate_of_turn);
    RUN_TEST(test_parses_trip_and_total_log);
    RUN_TEST(test_parses_battery_current_and_temperature);
    RUN_TEST(test_parses_propulsion_any_instance);
    RUN_TEST(test_propulsion_unrelated_leaf_ignored);
    RUN_TEST(test_parses_heading_magnetic_and_variation);
    RUN_TEST(test_touched_mask_single_field);
    RUN_TEST(test_touched_mask_multiple_fields_accumulate);
    RUN_TEST(test_touched_mask_survives_stale_accumulator);
    RUN_TEST(test_touched_mask_null_value_not_marked);
    RUN_TEST(test_touched_mask_unknown_path_not_marked);
    RUN_TEST(test_touched_mask_position_sets_lat_lon);
    RUN_TEST(test_touched_mask_apstate);
    RUN_TEST(test_context_other_vessel_routes_to_ais_not_view);
    RUN_TEST(test_context_other_vessel_sog_cog_merge);
    RUN_TEST(test_context_other_vessel_name_from_static_tree);
    RUN_TEST(test_context_self_urn_still_fills_view);
    RUN_TEST(test_context_self_id_matches_without_vessels_prefix);
    RUN_TEST(test_context_uuid_vessel_dropped);
    RUN_TEST(test_context_other_vessel_without_sinks_ignored);
    RUN_TEST(test_notification_upsert_from_delta);
    RUN_TEST(test_notification_normal_state_clears);
    RUN_TEST(test_notification_path_never_touches_view_mask);
    RUN_TEST(test_parse_hello_extracts_self);
    RUN_TEST(test_parses_engine_run_time);
    RUN_TEST(test_status_disconnected);
    RUN_TEST(test_status_live_during_warmup);
    RUN_TEST(test_status_stalled_after_warmup_no_activity);
    RUN_TEST(test_status_live_with_fresh_data);
    RUN_TEST(test_status_stalled_with_stale_data);
    RUN_TEST(test_status_reconnect_resets_warmup);
    RUN_TEST(test_status_handles_millis_wrap);
    RUN_TEST(test_status_live_when_frames_arrive_without_values);
    RUN_TEST(test_status_no_data_after_grace);
    RUN_TEST(test_status_live_within_no_data_grace);
    RUN_TEST(test_status_stalled_beats_no_data);
    RUN_TEST(test_status_stalled_when_all_signals_stale);
    RUN_TEST(test_status_frames_more_recent_than_values);
    RUN_TEST(test_status_default_threshold_is_30s);
    return UNITY_END();
}
