#include "source_signalk.h"

#include <math.h>

namespace boat {

namespace {

// Helper: publish if `v` is finite. Returns 1 on accept, 0 otherwise.
inline int pub(Field Snapshot::*field, uint32_t now_ms, double v) {
    if (!isfinite(v)) return 0;
    return publish(field, SourceKind::SignalK, now_ms, v) ? 1 : 0;
}

}  // namespace

void compose_from_boat(sk::Data &out, uint32_t now_ms) {
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
    out.depth = v(s.depth_m);
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
    out.apTargetHdg = v(s.autopilot_target_rad);
    out.currentSetTrue = v(s.current_set_rad);
    out.currentDrift = v(s.current_drift_mps);
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

int bridge_signalk_into_boat(const sk::Data &sk, uint32_t now_ms) {
    int n = 0;
    n += pub(&Snapshot::lat_deg, now_ms, sk.lat);
    n += pub(&Snapshot::lon_deg, now_ms, sk.lon);
    n += pub(&Snapshot::sog_mps, now_ms, sk.sog);
    n += pub(&Snapshot::stw_mps, now_ms, sk.stw);
    n += pub(&Snapshot::cog_true_rad, now_ms, sk.cogTrue);
    n += pub(&Snapshot::heading_true_rad, now_ms, sk.headingTrue);
    n += pub(&Snapshot::awa_rad, now_ms, sk.awa);
    n += pub(&Snapshot::aws_mps, now_ms, sk.aws);
    n += pub(&Snapshot::twa_rad, now_ms, sk.twa);
    n += pub(&Snapshot::tws_mps, now_ms, sk.tws);
    n += pub(&Snapshot::depth_m, now_ms, sk.depth);
    n += pub(&Snapshot::water_temp_k, now_ms, sk.waterTemp);
    n += pub(&Snapshot::battery_v, now_ms, sk.battVoltage);
    n += pub(&Snapshot::battery_soc, now_ms, sk.battSoc);
    n += pub(&Snapshot::tank_fuel, now_ms, sk.tankFuel);
    n += pub(&Snapshot::tank_water, now_ms, sk.tankWater);
    n += pub(&Snapshot::xte_m, now_ms, sk.xte);
    n += pub(&Snapshot::cts_rad, now_ms, sk.cts);
    n += pub(&Snapshot::btw_rad, now_ms, sk.btw);
    n += pub(&Snapshot::dtw_m, now_ms, sk.dtw);
    n += pub(&Snapshot::vmg_mps, now_ms, sk.vmg);
    n += pub(&Snapshot::autopilot_target_rad, now_ms, sk.apTargetHdg);
    n += pub(&Snapshot::current_set_rad, now_ms, sk.currentSetTrue);
    n += pub(&Snapshot::current_drift_mps, now_ms, sk.currentDrift);
    if (sk.apState[0] != 0) {
        if (publish_autopilot_state(SourceKind::SignalK, now_ms, sk.apState)) {
            n++;
        }
    }
    return n;
}

}  // namespace boat
