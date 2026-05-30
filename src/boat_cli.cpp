#include "boat_cli.h"

#include "boat_data.h"
#include "net.h"

namespace boat {

namespace {

void dump_field(const char *name, const Field &f, uint32_t now, uint32_t timeout) {
    bool is_fresh = fresh(f, now, timeout);
    net::logf("  %-22s = %12.4f  %-9s  age=%lums %s", name, f.value, source_name(f.source),
              (unsigned long)(f.updated_ms ? (now - f.updated_ms) : 0), is_fresh ? "" : "(STALE)");
}

}  // namespace

bool handleSerialCommand(const String &line) {
    if (!line.startsWith("boat")) return false;
    String rest = line.length() > 4 ? line.substring(4) : String("");
    rest.trim();

    if (rest == "priority") {
        Priority p = get_priority();
        String order;
        for (uint8_t i = 0; i < 5; ++i) {
            if (p.order[i] == SourceKind::None) break;
            if (order.length()) order += " > ";
            order += source_name(p.order[i]);
        }
        net::logf("[boat] priority: %s", order.c_str());
        Timeouts t = get_timeouts();
        net::logf("[boat] timeouts: n2k=%lums wifi=%lums sk=%lums demo=%lums",
                  (unsigned long)t.nmea2000_ms, (unsigned long)t.nmea_wifi_ms,
                  (unsigned long)t.signalk_ms, (unsigned long)t.demo_ms);
        return true;
    }
    if (rest == "reset") {
        reset_all();
        net::logf("[boat] all fields reset");
        return true;
    }
    if (rest.startsWith("timeout ")) {
        String args = rest.substring(8);
        args.trim();
        int sp = args.indexOf(' ');
        if (sp < 0) {
            net::logf("[boat] usage: boat timeout <n2k|wifi|sk|demo> <ms>");
            return true;
        }
        String src = args.substring(0, sp);
        uint32_t ms = (uint32_t)args.substring(sp + 1).toInt();
        Timeouts t = get_timeouts();
        if (src == "n2k")
            t.nmea2000_ms = ms;
        else if (src == "wifi")
            t.nmea_wifi_ms = ms;
        else if (src == "sk")
            t.signalk_ms = ms;
        else if (src == "demo")
            t.demo_ms = ms;
        else {
            net::logf("[boat] unknown source '%s'", src.c_str());
            return true;
        }
        set_timeouts(t);
        net::logf("[boat] timeout %s = %lums", src.c_str(), (unsigned long)ms);
        return true;
    }
    if (rest.length() == 0 || rest == "snapshot") {
        Snapshot s;
        copy_snapshot(s);
        Timeouts t = get_timeouts();
        uint32_t now = millis();
        net::logf("[boat] snapshot @ %lums:", (unsigned long)now);
        dump_field("lat_deg", s.lat_deg, now, timeout_for(t, s.lat_deg.source));
        dump_field("lon_deg", s.lon_deg, now, timeout_for(t, s.lon_deg.source));
        dump_field("sog_mps", s.sog_mps, now, timeout_for(t, s.sog_mps.source));
        dump_field("stw_mps", s.stw_mps, now, timeout_for(t, s.stw_mps.source));
        dump_field("cog_true_rad", s.cog_true_rad, now, timeout_for(t, s.cog_true_rad.source));
        dump_field("heading_true_rad", s.heading_true_rad, now,
                   timeout_for(t, s.heading_true_rad.source));
        dump_field("awa_rad", s.awa_rad, now, timeout_for(t, s.awa_rad.source));
        dump_field("aws_mps", s.aws_mps, now, timeout_for(t, s.aws_mps.source));
        dump_field("twa_rad", s.twa_rad, now, timeout_for(t, s.twa_rad.source));
        dump_field("tws_mps", s.tws_mps, now, timeout_for(t, s.tws_mps.source));
        dump_field("depth_m", s.depth_m, now, timeout_for(t, s.depth_m.source));
        dump_field("water_temp_k", s.water_temp_k, now, timeout_for(t, s.water_temp_k.source));
        dump_field("battery_v", s.battery_v, now, timeout_for(t, s.battery_v.source));
        dump_field("battery_soc", s.battery_soc, now, timeout_for(t, s.battery_soc.source));
        dump_field("xte_m", s.xte_m, now, timeout_for(t, s.xte_m.source));
        dump_field("btw_rad", s.btw_rad, now, timeout_for(t, s.btw_rad.source));
        dump_field("dtw_m", s.dtw_m, now, timeout_for(t, s.dtw_m.source));
        net::logf("  autopilot_state        = '%s'   %s   age=%lums", s.autopilot_state,
                  source_name(s.autopilot_state_source),
                  (unsigned long)(s.autopilot_state_updated_ms
                                      ? (now - s.autopilot_state_updated_ms)
                                      : 0));
        return true;
    }
    net::logf("[boat] usage: boat [snapshot|priority|reset|timeout <src> <ms>]");
    return true;
}

}  // namespace boat
