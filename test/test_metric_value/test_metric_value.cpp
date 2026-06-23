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
    RUN_TEST(test_fraction_soc);
    RUN_TEST(test_fraction_speed_range);
    RUN_TEST(test_fraction_unranged_source_is_nan);
    return UNITY_END();
}
