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

double metric_si(MetricSource src, const boat::View &d) {
    switch (src) {
    case MetricSource::AWS_kn:
        return d.aws;
    case MetricSource::TWS_kn:
        return d.tws;
    case MetricSource::SOG_kn:
        return d.sog;
    case MetricSource::STW_kn:
        return d.stw;
    case MetricSource::Depth_m:
        return d.depth;
    case MetricSource::DepthKeel_m:
        return d.depthKeel;
    case MetricSource::WaterTemp_C:
        return d.waterTemp;
    case MetricSource::BatteryV:
        return d.battVoltage;
    case MetricSource::BatterySOC_pct:
        return d.battSoc;
    case MetricSource::VMG_kn:
        return d.vmg;
    case MetricSource::VMGwind_kn:
        return d.vmgWind;
    case MetricSource::COG_deg:
        return d.cogTrue;
    case MetricSource::HDG_deg:
        return d.headingTrue;
    case MetricSource::AWA_deg:
        return d.awa;
    case MetricSource::TWA_deg:
        return d.twa;
    case MetricSource::BTW_deg:
        return d.btw;
    case MetricSource::CTS_deg:
        return d.cts;
    case MetricSource::XTE:
        return d.xte;
    case MetricSource::DTW:
        return d.dtw;
    case MetricSource::Rudder_deg:
        return d.rudder;
    case MetricSource::OutsideTemp_C:
        return d.outsideTemp;
    case MetricSource::OutsidePressure_hPa:
        return d.outsidePressure;
    case MetricSource::Humidity_pct:
        return d.humidity;
    case MetricSource::Roll_deg:
        return d.roll;
    case MetricSource::Pitch_deg:
        return d.pitch;
    case MetricSource::ROT_degmin:
        return d.rateOfTurn;
    case MetricSource::TripLog_nm:
        return d.tripLog;
    case MetricSource::Log_nm:
        return d.totalLog;
    case MetricSource::BattCurrent_A:
        return d.battCurrent;
    case MetricSource::BattTemp_C:
        return d.battTemp;
    case MetricSource::EngineRpm:
        return d.engineRevs;
    case MetricSource::EngineCoolant_C:
        return d.engineCoolantTemp;
    case MetricSource::EngineOilP_bar:
        return d.engineOilPressure;
    case MetricSource::EngineFuelRate_lph:
        return d.engineFuelRate;
    case MetricSource::HDGm_deg:
        return d.headingMag;
    case MetricSource::Variation_deg:
        return d.variation;
    case MetricSource::EngineHours_h:
        return d.engineHours;
    default:
        return NAN;
    }
}

double metric_display_from_si(MetricSource src, double v) {
    if (isnan(v)) return NAN;
    switch (src) {
    case MetricSource::AWS_kn:
    case MetricSource::TWS_kn:
    case MetricSource::SOG_kn:
    case MetricSource::STW_kn:
    case MetricSource::VMG_kn:
    case MetricSource::VMGwind_kn:
        return units::mps_to_kn(v);
    case MetricSource::Depth_m:
    case MetricSource::DepthKeel_m:
    case MetricSource::BatteryV:
    case MetricSource::XTE:
    case MetricSource::BattCurrent_A:  // signed A, no conversion
        return v;
    case MetricSource::WaterTemp_C:
    case MetricSource::OutsideTemp_C:
    case MetricSource::BattTemp_C:
    case MetricSource::EngineCoolant_C:
        return units::k_to_c(v);
    case MetricSource::BatterySOC_pct:
    case MetricSource::Humidity_pct:
        return v * 100.0;
    case MetricSource::COG_deg:
    case MetricSource::HDG_deg:
    case MetricSource::AWA_deg:
    case MetricSource::TWA_deg:
    case MetricSource::BTW_deg:
    case MetricSource::CTS_deg:
    case MetricSource::HDGm_deg:
        return rad_to_deg_pos(v);
    case MetricSource::DTW:
    case MetricSource::TripLog_nm:
    case MetricSource::Log_nm:
        return units::m_to_nm(v);  // nm
    case MetricSource::Rudder_deg:
    case MetricSource::Roll_deg:
    case MetricSource::Pitch_deg:
    case MetricSource::Variation_deg:  // signed, +E
        return units::rad_to_deg(v);   // signed deg
    case MetricSource::OutsidePressure_hPa:
        return units::pa_to_hpa(v);
    case MetricSource::ROT_degmin:
        return units::radps_to_degmin(v);
    case MetricSource::EngineRpm:
        return units::hz_to_rpm(v);
    case MetricSource::EngineOilP_bar:
        return units::pa_to_bar(v);
    case MetricSource::EngineFuelRate_lph:
        return units::m3s_to_lph(v);
    case MetricSource::EngineHours_h:
        return units::s_to_h(v);
    default:
        return NAN;
    }
}

double metric_value(MetricSource src, const boat::View &d) {
    return metric_display_from_si(src, metric_si(src, d));
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
    case MetricSource::OutsideTemp_C:
        return clamp01((v - (-10.0)) / (40.0 - (-10.0)));
    case MetricSource::OutsidePressure_hPa:
        // Barometer band: 960 (deep low) .. 1050 (strong high) hPa.
        return clamp01((v - 960.0) / (1050.0 - 960.0));
    case MetricSource::Humidity_pct:
        return clamp01(v / 100.0);
    case MetricSource::Roll_deg:
        // Centre-zero: ±45° heel maps onto 0..1 with 0.5 = upright.
        return clamp01((v + 45.0) / 90.0);
    case MetricSource::Pitch_deg:
        return clamp01((v + 30.0) / 60.0);  // centre-zero ±30°
    case MetricSource::ROT_degmin:
        return clamp01((v + 180.0) / 360.0);  // centre-zero ±180°/min
    case MetricSource::BattTemp_C:
        return clamp01(v / 60.0);
    case MetricSource::EngineRpm:
        return clamp01(v / 4000.0);
    case MetricSource::EngineCoolant_C:
        return clamp01(v / 120.0);
    case MetricSource::EngineOilP_bar:
        return clamp01(v / 6.0);
    case MetricSource::EngineFuelRate_lph:
        return clamp01(v / 20.0);
    default:
        return NAN;
    }
}

}  // namespace ui::layouts
