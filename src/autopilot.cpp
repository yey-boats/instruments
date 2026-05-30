#include "autopilot.h"

#include "storage.h"
#include <string.h>

#include "app_events.h"
#include "boat_data.h"
#include "net.h"
#include "signalk.h"

namespace autopilot {

namespace {

constexpr const char *NS = "ap";
Backend s_default_backend = Backend::SignalK;
// Spec 17 §6 permissions - conservative defaults match the plugin's
// default profile: engage off, standby+heading on.
Permissions s_perms;

void load_prefs() {
    storage::Namespace p(NS, true);
    s_default_backend = static_cast<Backend>(
        p.get_u8("backend", static_cast<uint8_t>(Backend::SignalK)));
    s_perms.allow_engage         = p.get_u8("eng", 0) != 0;
    s_perms.allow_standby        = p.get_u8("sby", 1) != 0;
    s_perms.allow_heading_adjust = p.get_u8("hdg", 1) != 0;
}

void save_prefs() {
    storage::Namespace p(NS, false);
    p.put_u8("backend", static_cast<uint8_t>(s_default_backend));
    p.put_u8("eng", s_perms.allow_engage ? 1 : 0);
    p.put_u8("sby", s_perms.allow_standby ? 1 : 0);
    p.put_u8("hdg", s_perms.allow_heading_adjust ? 1 : 0);
}

// SignalK backend. Posts to the net worker queue (app::post_net) so
// callers on the LVGL task (button handlers, screen callbacks) never
// block on HTTPClient. The net worker drains SignalKPut and runs
// sk::putValue itself.
//
// Return is "queued" not "delivered" - the actual PUT status is
// logged by the worker. UIs reflect actual state from the next
// subscription update or heartbeat.
Result post_sk_put(const char *path, const char *body) {
    app::Command c;
    c.type = app::CommandType::SignalKPut;
    strncpy(c.a, path, sizeof(c.a) - 1);
    strncpy(c.b, body, sizeof(c.b) - 1);
    return app::post_net(c, 100) ? Result::Ok : Result::Failed;
}

Result sk_set_mode(Mode m) {
    const char *name = mode_name(m);
    if (!name || strcmp(name, "?") == 0) return Result::InvalidPayload;
    String body = String("\"") + name + "\"";
    return post_sk_put("steering/autopilot/state", body.c_str());
}

Result sk_adjust(int delta_deg) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", delta_deg);
    return post_sk_put("steering/autopilot/actions/adjustHeading", buf);
}

Result sk_silence() {
    return post_sk_put("notifications/silence", "\"silenced\"");
}

#ifdef ENABLE_NMEA2000
// Placeholder. Per spec 12 §4 safety rule: "Add a dry-run/sniffer
// mode before enabling transmit." Until the dry-run validation work
// lands, every write returns BackendUnavailable so the UI can show
// a "backend not yet authorised" error rather than silently
// succeeding.
Result n2k_set_mode(Mode) { return Result::BackendUnavailable; }
Result n2k_adjust(int)    { return Result::BackendUnavailable; }
Result n2k_silence()      { return Result::BackendUnavailable; }
#endif

}  // namespace

// mode_name, mode_from_string, backend_name, result_name are pure
// helpers and live in autopilot_pure.cpp so the host test env can
// link them without pulling in Arduino dependencies.

Backend default_backend() { return s_default_backend; }
void set_default_backend(Backend b) {
    s_default_backend = b;
    save_prefs();
    net::logf("[ap] backend = %s", backend_name(b));
}

Permissions get_permissions() { return s_perms; }
void set_permissions(const Permissions &p) {
    s_perms = p;
    save_prefs();
    net::logf("[ap] permissions: engage=%d standby=%d heading=%d",
              s_perms.allow_engage, s_perms.allow_standby,
              s_perms.allow_heading_adjust);
}

void copy_state(State &out) {
    boat::Snapshot s;
    boat::copy_snapshot(s);
    out.backend = s_default_backend;
    out.mode = mode_from_string(s.autopilot_state);
    out.target_heading_rad = boat::value_or_nan(
        s.autopilot_target_rad,
        millis(),
        boat::timeout_for(boat::get_timeouts(), s.autopilot_target_rad.source));
    out.current_heading_rad = boat::value_or_nan(
        s.heading_true_rad,
        millis(),
        boat::timeout_for(boat::get_timeouts(), s.heading_true_rad.source));
    out.target_wind_angle_rad = NAN;
    out.alarm_active = false;
    out.warning_active = false;
    out.alarm_text[0] = 0;
}

Result set_mode(Mode m) {
    if (!mode_allowed(m, s_perms)) return Result::Forbidden;
    switch (s_default_backend) {
        case Backend::SignalK:           return sk_set_mode(m);
#ifdef ENABLE_NMEA2000
        case Backend::NMEA2000Raymarine: return n2k_set_mode(m);
#else
        case Backend::NMEA2000Raymarine: return Result::BackendUnavailable;
#endif
    }
    return Result::Failed;
}

Result adjust_heading_deg(int delta) {
    if (delta < -90 || delta > 90) return Result::InvalidPayload;
    if (!s_perms.allow_heading_adjust) return Result::Forbidden;
    switch (s_default_backend) {
        case Backend::SignalK:           return sk_adjust(delta);
#ifdef ENABLE_NMEA2000
        case Backend::NMEA2000Raymarine: return n2k_adjust(delta);
#else
        case Backend::NMEA2000Raymarine: return Result::BackendUnavailable;
#endif
    }
    return Result::Failed;
}

Result silence_alarm() {
    switch (s_default_backend) {
        case Backend::SignalK:           return sk_silence();
#ifdef ENABLE_NMEA2000
        case Backend::NMEA2000Raymarine: return n2k_silence();
#else
        case Backend::NMEA2000Raymarine: return Result::BackendUnavailable;
#endif
    }
    return Result::Failed;
}

void setup() {
    load_prefs();
    net::logf("[ap] default backend = %s", backend_name(s_default_backend));
}


bool handleSerialCommand(const String &line) {
    if (!line.startsWith("autopilot") && !line.startsWith("ap-")) return false;

    if (line == "autopilot" || line == "autopilot status") {
        State st;
        copy_state(st);
        net::logf("[ap] backend=%s mode=%s hdg=%.1f target=%.1f alarm=%d",
                  backend_name(st.backend), mode_name(st.mode),
                  isnan(st.current_heading_rad) ? 0.0 :
                      st.current_heading_rad * 180.0 / M_PI,
                  isnan(st.target_heading_rad) ? 0.0 :
                      st.target_heading_rad * 180.0 / M_PI,
                  st.alarm_active);
        return true;
    }
    if (line.startsWith("autopilot mode ")) {
        String m = line.substring(15);
        m.trim();
        Result r = set_mode(mode_from_string(m.c_str()));
        net::logf("[ap] set_mode(%s) -> %s", m.c_str(), result_name(r));
        return true;
    }
    if (line.startsWith("autopilot heading ")) {
        int delta = (int)line.substring(18).toInt();
        Result r = adjust_heading_deg(delta);
        net::logf("[ap] adjust(%+d deg) -> %s", delta, result_name(r));
        return true;
    }
    if (line == "autopilot silence") {
        Result r = silence_alarm();
        net::logf("[ap] silence -> %s", result_name(r));
        return true;
    }
    if (line.startsWith("autopilot backend ")) {
        String b = line.substring(18);
        b.trim();
        if (b == "signalk") {
            set_default_backend(Backend::SignalK);
        } else if (b == "nmea2000") {
            set_default_backend(Backend::NMEA2000Raymarine);
        } else {
            net::logf("[ap] backend must be signalk|nmea2000");
        }
        return true;
    }
    net::logf("[ap] usage: autopilot [status|mode <m>|heading <delta>|silence|backend <b>]");
    return true;
}

}  // namespace autopilot
