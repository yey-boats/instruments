// Pure helpers for the autopilot module. No Arduino dependencies so
// the native test env can link this TU alone.

#include "autopilot.h"

#include <string.h>

namespace autopilot {

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

const char *result_name(Result r) {
    switch (r) {
        case Result::Ok:                 return "ok";
        case Result::InvalidPayload:     return "invalid";
        case Result::BackendUnavailable: return "backend-unavailable";
        case Result::Failed:             return "failed";
    }
    return "?";
}

}  // namespace autopilot
