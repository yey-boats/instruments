#include "autopilot.h"

#include <Preferences.h>
#include <string.h>

#include "boat_data.h"
#include "net.h"
#include "signalk.h"

namespace autopilot {

namespace {

constexpr const char *NS = "ap";
Backend s_default_backend = Backend::SignalK;

void load_prefs() {
    Preferences p;
    p.begin(NS, true);
    s_default_backend = static_cast<Backend>(
        p.getUChar("backend", static_cast<uint8_t>(Backend::SignalK)));
    p.end();
}

void save_prefs() {
    Preferences p;
    p.begin(NS, false);
    p.putUChar("backend", static_cast<uint8_t>(s_default_backend));
    p.end();
}

// SignalK backend. Reuses the existing PUT path - same code that the
// sk-ap-state / sk-ap-adjust CLIs drive. Caller runs on a non-LVGL
// task (console handler is on the LVGL task today, but putValue runs
// HTTPClient which is fast enough at 3 s timeout to be acceptable;
// future revision should post to the net worker queue per spec).
Result sk_set_mode(Mode m) {
    const char *name = mode_name(m);
    if (!name || strcmp(name, "?") == 0) return Result::InvalidPayload;
    String body = String("\"") + name + "\"";
    int rc = sk::putValue("steering/autopilot/state", body.c_str());
    return (rc >= 200 && rc < 300) ? Result::Ok : Result::Failed;
}

Result sk_adjust(int delta_deg) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", delta_deg);
    int rc = sk::putValue("steering/autopilot/actions/adjustHeading", buf);
    return (rc >= 200 && rc < 300) ? Result::Ok : Result::Failed;
}

Result sk_silence() {
    // SignalK alarm silence path: notifications.silence (V1 API).
    int rc = sk::putValue("notifications/silence", "\"silenced\"");
    return (rc >= 200 && rc < 300) ? Result::Ok : Result::Failed;
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

const char *mode_name(Mode m) {
    switch (m) {
        case Mode::Standby:  return "standby";
        case Mode::Auto:     return "auto";
        case Mode::Wind:     return "wind";
        case Mode::PreTrack: return "pretrack";
        case Mode::Track:    return "track";
        case Mode::Unknown:  return "unknown";
    }
    return "?";
}

Mode mode_from_string(const char *s) {
    if (!s) return Mode::Unknown;
    if (!strcmp(s, "standby"))  return Mode::Standby;
    if (!strcmp(s, "auto"))     return Mode::Auto;
    if (!strcmp(s, "wind"))     return Mode::Wind;
    if (!strcmp(s, "pretrack")) return Mode::PreTrack;
    if (!strcmp(s, "track"))    return Mode::Track;
    return Mode::Unknown;
}

const char *backend_name(Backend b) {
    switch (b) {
        case Backend::SignalK:           return "signalk";
        case Backend::NMEA2000Raymarine: return "nmea2000";
    }
    return "?";
}

Backend default_backend() { return s_default_backend; }
void set_default_backend(Backend b) {
    s_default_backend = b;
    save_prefs();
    net::logf("[ap] backend = %s", backend_name(b));
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

static const char *result_name(Result r) {
    switch (r) {
        case Result::Ok:                 return "ok";
        case Result::InvalidPayload:     return "invalid";
        case Result::BackendUnavailable: return "backend-unavailable";
        case Result::Failed:             return "failed";
    }
    return "?";
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
