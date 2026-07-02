#include <math.h>

#include <unity.h>

#include "metric_value.h"

using ui::layouts::metric_unit_fraction;
using ui::layouts::metric_value;
using ui::layouts::MetricSource;

static boat::View blank() {
    return boat::View{};  // all fields NaN/zero per in-class initializers
}

// ---- metric_value: SI -> display unit conversions --------------------------

static void test_speed_mps_to_knots() {
    boat::View d = blank();
    d.sog = 5.0;  // m/s
    // 5 m/s / (1852/3600) = 9.7192... kn
    TEST_ASSERT_FLOAT_WITHIN(0.01, 9.7192, metric_value(MetricSource::SOG_kn, d));
    d.stw = 2.5;
    TEST_ASSERT_FLOAT_WITHIN(0.01, 4.8596, metric_value(MetricSource::STW_kn, d));
}

static void test_angle_rad_to_deg_positive_range() {
    boat::View d = blank();
    d.headingTrue = M_PI;  // 180 deg
    TEST_ASSERT_FLOAT_WITHIN(0.01, 180.0, metric_value(MetricSource::HDG_deg, d));
    // negative apparent wind angle wraps into [0,360)
    d.awa = -M_PI / 2.0;  // -90 -> 270
    TEST_ASSERT_FLOAT_WITHIN(0.01, 270.0, metric_value(MetricSource::AWA_deg, d));
}

static void test_rudder_is_signed_degrees() {
    boat::View d = blank();
    d.rudder = -M_PI / 6.0;  // -30 deg, stays signed (no wrap)
    TEST_ASSERT_FLOAT_WITHIN(0.01, -30.0, metric_value(MetricSource::Rudder_deg, d));
}

static void test_temp_kelvin_to_celsius() {
    boat::View d = blank();
    d.waterTemp = 295.15;  // K -> 22 C
    TEST_ASSERT_FLOAT_WITHIN(0.01, 22.0, metric_value(MetricSource::WaterTemp_C, d));
}

static void test_soc_fraction_to_percent() {
    boat::View d = blank();
    d.battSoc = 0.83;
    TEST_ASSERT_FLOAT_WITHIN(0.01, 83.0, metric_value(MetricSource::BatterySOC_pct, d));
}

static void test_dtw_metres_to_nm() {
    boat::View d = blank();
    d.dtw = 1852.0 * 3.0;  // 3 nm
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.0, metric_value(MetricSource::DTW, d));
}

static void test_passthrough_units() {
    boat::View d = blank();
    d.depth = 4.2;  // m, no conversion
    d.battVoltage = 12.6;
    d.xte = -15.0;  // m, signed
    TEST_ASSERT_FLOAT_WITHIN(0.001, 4.2, metric_value(MetricSource::Depth_m, d));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.6, metric_value(MetricSource::BatteryV, d));
    TEST_ASSERT_FLOAT_WITHIN(0.001, -15.0, metric_value(MetricSource::XTE, d));
}

static void test_missing_field_is_nan() {
    boat::View d = blank();  // sog is NaN
    TEST_ASSERT_TRUE(isnan(metric_value(MetricSource::SOG_kn, d)));
    TEST_ASSERT_TRUE(isnan(metric_value(MetricSource::HDG_deg, d)));
}

static void test_nonscalar_source_is_nan() {
    boat::View d = blank();
    d.sog = 5.0;
    TEST_ASSERT_TRUE(isnan(metric_value(MetricSource::None, d)));
}

// ---- coverage wave: new sources ---------------------------------------------

static void test_engine_rpm_hz_to_rpm() {
    boat::View d = blank();
    d.engineRevs = 30.0;  // Hz
    TEST_ASSERT_FLOAT_WITHIN(0.01, 1800.0, metric_value(MetricSource::EngineRpm, d));
}

static void test_engine_oil_pressure_pa_to_bar() {
    boat::View d = blank();
    d.engineOilPressure = 350000.0;  // Pa
    // MIDL library engine screen formats OIL in bar (1 dp).
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.5, metric_value(MetricSource::EngineOilP_bar, d));
}

static void test_engine_coolant_k_to_c() {
    boat::View d = blank();
    d.engineCoolantTemp = 361.15;
    TEST_ASSERT_FLOAT_WITHIN(0.01, 88.0, metric_value(MetricSource::EngineCoolant_C, d));
}

static void test_engine_fuel_rate_m3s_to_lph() {
    boat::View d = blank();
    d.engineFuelRate = 5.2e-3 / 3600.0;  // 5.2 L/h in m3/s
    TEST_ASSERT_FLOAT_WITHIN(0.001, 5.2, metric_value(MetricSource::EngineFuelRate_lph, d));
}

static void test_outside_pressure_pa_to_hpa() {
    boat::View d = blank();
    d.outsidePressure = 101300.0;
    TEST_ASSERT_FLOAT_WITHIN(0.01, 1013.0, metric_value(MetricSource::OutsidePressure_hPa, d));
}

static void test_outside_temp_and_humidity() {
    boat::View d = blank();
    d.outsideTemp = 291.15;
    d.humidity = 0.62;
    TEST_ASSERT_FLOAT_WITHIN(0.01, 18.0, metric_value(MetricSource::OutsideTemp_C, d));
    TEST_ASSERT_FLOAT_WITHIN(0.01, 62.0, metric_value(MetricSource::Humidity_pct, d));
}

static void test_roll_pitch_signed_degrees() {
    boat::View d = blank();
    d.roll = -0.0872;  // ~-5 deg (port heel) — stays signed, no [0,360) wrap
    d.pitch = 0.0349;  // ~+2 deg bow up
    TEST_ASSERT_FLOAT_WITHIN(0.05, -5.0, metric_value(MetricSource::Roll_deg, d));
    TEST_ASSERT_FLOAT_WITHIN(0.05, 2.0, metric_value(MetricSource::Pitch_deg, d));
}

static void test_rate_of_turn_radps_to_degmin() {
    boat::View d = blank();
    d.rateOfTurn = M_PI / 180.0;  // 1 deg/s = 60 deg/min
    TEST_ASSERT_FLOAT_WITHIN(0.01, 60.0, metric_value(MetricSource::ROT_degmin, d));
    d.rateOfTurn = -M_PI / 180.0;  // signed (port turn)
    TEST_ASSERT_FLOAT_WITHIN(0.01, -60.0, metric_value(MetricSource::ROT_degmin, d));
}

static void test_trip_and_total_log_m_to_nm() {
    boat::View d = blank();
    d.tripLog = 9260.0;      // 5 nm
    d.totalLog = 1852000.0;  // 1000 nm
    TEST_ASSERT_FLOAT_WITHIN(0.001, 5.0, metric_value(MetricSource::TripLog_nm, d));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1000.0, metric_value(MetricSource::Log_nm, d));
}

static void test_battery_current_signed_and_temp() {
    boat::View d = blank();
    d.battCurrent = -12.4;  // discharging: sign preserved
    d.battTemp = 298.15;
    TEST_ASSERT_FLOAT_WITHIN(0.001, -12.4, metric_value(MetricSource::BattCurrent_A, d));
    TEST_ASSERT_FLOAT_WITHIN(0.01, 25.0, metric_value(MetricSource::BattTemp_C, d));
}

static void test_heading_magnetic_and_variation() {
    boat::View d = blank();
    d.headingMag = -M_PI / 2.0;  // wraps into [0,360) like HDG_deg
    d.variation = 0.05;          // rad, +E — stays signed
    TEST_ASSERT_FLOAT_WITHIN(0.01, 270.0, metric_value(MetricSource::HDGm_deg, d));
    TEST_ASSERT_FLOAT_WITHIN(0.01, 2.86, metric_value(MetricSource::Variation_deg, d));
}

// ---- metric_unit_fraction: gauge/bar fill ----------------------------------

static void test_fraction_soc() {
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.5, metric_unit_fraction(MetricSource::BatterySOC_pct, 50.0));
    // clamps above 100%
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.0, metric_unit_fraction(MetricSource::BatterySOC_pct, 130.0));
}

static void test_fraction_speed_range() {
    // SOG range is 0..15 kn
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.5, metric_unit_fraction(MetricSource::SOG_kn, 7.5));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, metric_unit_fraction(MetricSource::SOG_kn, -3.0));
}

static void test_fraction_unranged_source_is_nan() {
    // Headings have no obvious gauge range.
    TEST_ASSERT_TRUE(isnan(metric_unit_fraction(MetricSource::HDG_deg, 180.0)));
    TEST_ASSERT_TRUE(isnan(metric_unit_fraction(MetricSource::SOG_kn, NAN)));
}

static void test_fraction_new_sources() {
    // Engine RPM heuristic range 0..4000.
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.45, metric_unit_fraction(MetricSource::EngineRpm, 1800.0));
    // Roll is centre-zero on ±45°: upright = 0.5.
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.5, metric_unit_fraction(MetricSource::Roll_deg, 0.0));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, metric_unit_fraction(MetricSource::Roll_deg, -50.0));
    // Barometer band 960..1050 hPa.
    TEST_ASSERT_FLOAT_WITHIN(0.01, 0.59,
                             metric_unit_fraction(MetricSource::OutsidePressure_hPa, 1013.0));
    // Trip log / variation have no obvious range.
    TEST_ASSERT_TRUE(isnan(metric_unit_fraction(MetricSource::TripLog_nm, 5.0)));
    TEST_ASSERT_TRUE(isnan(metric_unit_fraction(MetricSource::Variation_deg, 3.0)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_speed_mps_to_knots);
    RUN_TEST(test_angle_rad_to_deg_positive_range);
    RUN_TEST(test_rudder_is_signed_degrees);
    RUN_TEST(test_temp_kelvin_to_celsius);
    RUN_TEST(test_soc_fraction_to_percent);
    RUN_TEST(test_dtw_metres_to_nm);
    RUN_TEST(test_passthrough_units);
    RUN_TEST(test_missing_field_is_nan);
    RUN_TEST(test_nonscalar_source_is_nan);
    RUN_TEST(test_engine_rpm_hz_to_rpm);
    RUN_TEST(test_engine_oil_pressure_pa_to_bar);
    RUN_TEST(test_engine_coolant_k_to_c);
    RUN_TEST(test_engine_fuel_rate_m3s_to_lph);
    RUN_TEST(test_outside_pressure_pa_to_hpa);
    RUN_TEST(test_outside_temp_and_humidity);
    RUN_TEST(test_roll_pitch_signed_degrees);
    RUN_TEST(test_rate_of_turn_radps_to_degmin);
    RUN_TEST(test_trip_and_total_log_m_to_nm);
    RUN_TEST(test_battery_current_signed_and_temp);
    RUN_TEST(test_heading_magnetic_and_variation);
    RUN_TEST(test_fraction_soc);
    RUN_TEST(test_fraction_speed_range);
    RUN_TEST(test_fraction_unranged_source_is_nan);
    RUN_TEST(test_fraction_new_sources);
    return UNITY_END();
}
