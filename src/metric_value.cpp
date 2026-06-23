#include "metric_value.h"

#include <math.h>

#include "units.h"

namespace ui::layouts {

namespace {

// Heading/angle in radians -> degrees in [0, 360). Mirrors the historical
// ui::rad_to_deg_pos (ui_data.h) exactly: a single +360 fixup, which fully
// normalises the [-pi, pi] / [0, 2pi) inputs these metrics carry.
double rad_to_deg_pos(double rad) {
    double d = units::rad_to_deg(rad);
    if (d < 0) d += 360;
    return d;
}

}  // namespace

double metric_value(MetricSource src, const boat::View &d) {
    switch (src) {
    case MetricSource::AWS_kn:
        return isnan(d.aws) ? NAN : units::mps_to_kn(d.aws);
    case MetricSource::TWS_kn:
        return isnan(d.tws) ? NAN : units::mps_to_kn(d.tws);
    case MetricSource::SOG_kn:
        return isnan(d.sog) ? NAN : units::mps_to_kn(d.sog);
    case MetricSource::STW_kn:
        return isnan(d.stw) ? NAN : units::mps_to_kn(d.stw);
    case MetricSource::Depth_m:
        return d.depth;
    case MetricSource::DepthKeel_m:
        return d.depthKeel;
    case MetricSource::WaterTemp_C:
        return isnan(d.waterTemp) ? NAN : units::k_to_c(d.waterTemp);
    case MetricSource::BatteryV:
        return d.battVoltage;
    case MetricSource::BatterySOC_pct:
        return isnan(d.battSoc) ? NAN : d.battSoc * 100.0;
    case MetricSource::VMG_kn:
        return isnan(d.vmg) ? NAN : units::mps_to_kn(d.vmg);
    case MetricSource::VMGwind_kn:
        return isnan(d.vmgWind) ? NAN : units::mps_to_kn(d.vmgWind);
    case MetricSource::COG_deg:
        return isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    case MetricSource::HDG_deg:
        return isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
    case MetricSource::AWA_deg:
        return isnan(d.awa) ? NAN : rad_to_deg_pos(d.awa);
    case MetricSource::TWA_deg:
        return isnan(d.twa) ? NAN : rad_to_deg_pos(d.twa);
    case MetricSource::BTW_deg:
        return isnan(d.btw) ? NAN : rad_to_deg_pos(d.btw);
    case MetricSource::CTS_deg:
        return isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    case MetricSource::XTE:
        return d.xte;
    case MetricSource::DTW:
        return isnan(d.dtw) ? NAN : units::m_to_nm(d.dtw);  // nm
    case MetricSource::Rudder_deg:
        return isnan(d.rudder) ? NAN : units::rad_to_deg(d.rudder);  // signed deg
    default:
        return NAN;
    }
}

double metric_unit_fraction(MetricSource src, double v) {
    if (isnan(v)) return NAN;
    auto clamp01 = [](double x) {
        if (x < 0) return 0.0;
        if (x > 1) return 1.0;
        return x;
    };
    switch (src) {
    case MetricSource::BatterySOC_pct:
        return clamp01(v / 100.0);
    case MetricSource::BatteryV:
        return clamp01((v - 11.0) / (14.4 - 11.0));
    case MetricSource::Depth_m:
    case MetricSource::DepthKeel_m:
        return clamp01(v / 30.0);
    case MetricSource::AWS_kn:
        return clamp01(v / 40.0);
    case MetricSource::TWS_kn:
        return clamp01(v / 40.0);
    case MetricSource::SOG_kn:
        return clamp01(v / 15.0);
    case MetricSource::STW_kn:
        return clamp01(v / 15.0);
    case MetricSource::VMG_kn:
    case MetricSource::VMGwind_kn:
        return clamp01(v / 15.0);
    case MetricSource::WaterTemp_C:
        return clamp01((v - 5.0) / (30.0 - 5.0));
    default:
        return NAN;
    }
}

}  // namespace ui::layouts
