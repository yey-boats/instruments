#include "source_signalk.h"

#include <math.h>

namespace boat {

namespace {

// Helper: publish if this delta touched `id` and `v` is finite. Returns 1
// on accept, 0 otherwise. The mask gate is what preserves per-field
// staleness (see ingest_signalk doc comment in source_signalk.h).
inline int pub(Field Snapshot::*field, FieldId id, FieldMask touched, uint32_t now_ms, double v) {
    if (!(touched & field_bit(id))) return 0;
    if (!isfinite(v)) return 0;
    return publish(field, SourceKind::SignalK, now_ms, v) ? 1 : 0;
}

}  // namespace

void compose(View &out, uint32_t now_ms) {
    Snapshot s;
    copy_snapshot(s);
    Timeouts t = get_timeouts();
    auto v = [&](const Field &f) -> double {
        return value_or_nan(f, now_ms, timeout_for(t, f.source));
    };
    out.lat = v(s.lat_deg);
    out.lon = v(s.lon_deg);
    out.sog = v(s.sog_mps);
    out.stw = v(s.stw_mps);
    out.cogTrue = v(s.cog_true_rad);
    out.headingTrue = v(s.heading_true_rad);
    out.awa = v(s.awa_rad);
    out.aws = v(s.aws_mps);
    out.twa = v(s.twa_rad);
    out.tws = v(s.tws_mps);
    out.beatAngle = v(s.beat_angle_rad);
    out.gybeAngle = v(s.gybe_angle_rad);
    out.depth = v(s.depth_m);
    out.depthKeel = v(s.depth_keel_m);
    out.waterTemp = v(s.water_temp_k);
    out.battVoltage = v(s.battery_v);
    out.battSoc = v(s.battery_soc);
    out.tankFuel = v(s.tank_fuel);
    out.tankWater = v(s.tank_water);
    out.xte = v(s.xte_m);
    out.cts = v(s.cts_rad);
    out.btw = v(s.btw_rad);
    out.dtw = v(s.dtw_m);
    out.vmg = v(s.vmg_mps);
    out.vmgWind = v(s.vmg_wind_mps);
    out.rudder = v(s.rudder_angle_rad);
    out.apTargetHdg = v(s.autopilot_target_rad);
    out.currentSetTrue = v(s.current_set_rad);
    out.currentDrift = v(s.current_drift_mps);
    out.headingMag = v(s.heading_mag_rad);
    out.variation = v(s.variation_rad);
    out.roll = v(s.roll_rad);
    out.pitch = v(s.pitch_rad);
    out.rateOfTurn = v(s.rate_of_turn_radps);
    out.outsideTemp = v(s.outside_temp_k);
    out.outsidePressure = v(s.outside_pressure_pa);
    out.humidity = v(s.humidity_ratio);
    out.battCurrent = v(s.battery_current_a);
    out.battTemp = v(s.battery_temp_k);
    out.engineRevs = v(s.engine_rev_hz);
    out.engineCoolantTemp = v(s.engine_coolant_temp_k);
    out.engineOilPressure = v(s.engine_oil_pressure_pa);
    out.engineFuelRate = v(s.engine_fuel_rate_m3s);
    out.tripLog = v(s.trip_log_m);
    out.totalLog = v(s.total_log_m);
    Field ap_field;
    ap_field.value = 0.0;
    ap_field.updated_ms = s.autopilot_state_updated_ms;
    ap_field.source = s.autopilot_state_source;
    if (fresh(ap_field, now_ms, timeout_for(t, s.autopilot_state_source))) {
        size_t n = sizeof(out.apState);
        for (size_t i = 0; i < n - 1; ++i)
            out.apState[i] = s.autopilot_state[i];
        out.apState[n - 1] = 0;
    } else {
        out.apState[0] = 0;
    }
}

int ingest_signalk(const View &v, uint32_t now_ms, FieldMask touched) {
    if (touched == 0) return 0;
    using FI = FieldId;
    int n = 0;
    n += pub(&Snapshot::lat_deg, FI::Lat, touched, now_ms, v.lat);
    n += pub(&Snapshot::lon_deg, FI::Lon, touched, now_ms, v.lon);
    n += pub(&Snapshot::sog_mps, FI::Sog, touched, now_ms, v.sog);
    n += pub(&Snapshot::stw_mps, FI::Stw, touched, now_ms, v.stw);
    n += pub(&Snapshot::cog_true_rad, FI::CogTrue, touched, now_ms, v.cogTrue);
    n += pub(&Snapshot::heading_true_rad, FI::HeadingTrue, touched, now_ms, v.headingTrue);
    n += pub(&Snapshot::awa_rad, FI::Awa, touched, now_ms, v.awa);
    n += pub(&Snapshot::aws_mps, FI::Aws, touched, now_ms, v.aws);
    n += pub(&Snapshot::twa_rad, FI::Twa, touched, now_ms, v.twa);
    n += pub(&Snapshot::tws_mps, FI::Tws, touched, now_ms, v.tws);
    n += pub(&Snapshot::beat_angle_rad, FI::BeatAngle, touched, now_ms, v.beatAngle);
    n += pub(&Snapshot::gybe_angle_rad, FI::GybeAngle, touched, now_ms, v.gybeAngle);
    n += pub(&Snapshot::depth_m, FI::Depth, touched, now_ms, v.depth);
    n += pub(&Snapshot::depth_keel_m, FI::DepthKeel, touched, now_ms, v.depthKeel);
    n += pub(&Snapshot::water_temp_k, FI::WaterTemp, touched, now_ms, v.waterTemp);
    n += pub(&Snapshot::battery_v, FI::BattVoltage, touched, now_ms, v.battVoltage);
    n += pub(&Snapshot::battery_soc, FI::BattSoc, touched, now_ms, v.battSoc);
    n += pub(&Snapshot::tank_fuel, FI::TankFuel, touched, now_ms, v.tankFuel);
    n += pub(&Snapshot::tank_water, FI::TankWater, touched, now_ms, v.tankWater);
    n += pub(&Snapshot::xte_m, FI::Xte, touched, now_ms, v.xte);
    n += pub(&Snapshot::cts_rad, FI::Cts, touched, now_ms, v.cts);
    n += pub(&Snapshot::btw_rad, FI::Btw, touched, now_ms, v.btw);
    n += pub(&Snapshot::dtw_m, FI::Dtw, touched, now_ms, v.dtw);
    n += pub(&Snapshot::vmg_mps, FI::Vmg, touched, now_ms, v.vmg);
    n += pub(&Snapshot::vmg_wind_mps, FI::VmgWind, touched, now_ms, v.vmgWind);
    n += pub(&Snapshot::rudder_angle_rad, FI::Rudder, touched, now_ms, v.rudder);
    n += pub(&Snapshot::autopilot_target_rad, FI::ApTargetHdg, touched, now_ms, v.apTargetHdg);
    n += pub(&Snapshot::current_set_rad, FI::CurrentSet, touched, now_ms, v.currentSetTrue);
    n += pub(&Snapshot::current_drift_mps, FI::CurrentDrift, touched, now_ms, v.currentDrift);
    n += pub(&Snapshot::heading_mag_rad, FI::HeadingMag, touched, now_ms, v.headingMag);
    n += pub(&Snapshot::variation_rad, FI::Variation, touched, now_ms, v.variation);
    n += pub(&Snapshot::roll_rad, FI::Roll, touched, now_ms, v.roll);
    n += pub(&Snapshot::pitch_rad, FI::Pitch, touched, now_ms, v.pitch);
    n += pub(&Snapshot::rate_of_turn_radps, FI::RateOfTurn, touched, now_ms, v.rateOfTurn);
    n += pub(&Snapshot::outside_temp_k, FI::OutsideTemp, touched, now_ms, v.outsideTemp);
    n += pub(&Snapshot::outside_pressure_pa, FI::OutsidePressure, touched, now_ms,
             v.outsidePressure);
    n += pub(&Snapshot::humidity_ratio, FI::Humidity, touched, now_ms, v.humidity);
    n += pub(&Snapshot::battery_current_a, FI::BattCurrent, touched, now_ms, v.battCurrent);
    n += pub(&Snapshot::battery_temp_k, FI::BattTemp, touched, now_ms, v.battTemp);
    n += pub(&Snapshot::engine_rev_hz, FI::EngineRpm, touched, now_ms, v.engineRevs);
    n += pub(&Snapshot::engine_coolant_temp_k, FI::EngineCoolantTemp, touched, now_ms,
             v.engineCoolantTemp);
    n += pub(&Snapshot::engine_oil_pressure_pa, FI::EngineOilPressure, touched, now_ms,
             v.engineOilPressure);
    n +=
        pub(&Snapshot::engine_fuel_rate_m3s, FI::EngineFuelRate, touched, now_ms, v.engineFuelRate);
    n += pub(&Snapshot::trip_log_m, FI::TripLog, touched, now_ms, v.tripLog);
    n += pub(&Snapshot::total_log_m, FI::TotalLog, touched, now_ms, v.totalLog);
    if ((touched & field_bit(FI::ApState)) && v.apState[0] != 0) {
        if (publish_autopilot_state(SourceKind::SignalK, now_ms, v.apState)) {
            n++;
        }
    }
    return n;
}

}  // namespace boat
