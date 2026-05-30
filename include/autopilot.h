#pragma once

// Autopilot backend interface per docs/specs/12 §4.
//
// Screens / commands talk to autopilot:: and never to the underlying
// transport. Today there are two backends:
//
//   - SignalK   - PUTs to steering/autopilot/* over the existing SK
//                 REST path (works with the signalk-autopilot
//                 emulator from spec 16; verified end-to-end).
//   - NMEA2000  - Raymarine EVO via CAN. Placeholder behind
//                 -DENABLE_NMEA2000; rejects writes (returns
//                 BackendUnavailable) until hardware is wired and
//                 the implementation lands.
//
// Selection is per-call: caller chooses the backend explicitly or
// uses the configured default. The split lets the autopilot screen
// run an integration test against SignalK while leaving the N2k
// surface dormant.

#include <math.h>
#include <stdint.h>

// Arduino-only types (`String`, `Preferences`). The pure helpers +
// enums declared below compile cleanly on the native host test env
// without these.
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace autopilot {

enum class Backend : uint8_t {
    SignalK = 0,
    NMEA2000Raymarine,
};

enum class Mode : uint8_t {
    Unknown = 0,
    Standby,
    Auto,
    Wind,
    PreTrack,
    Track,
};

enum class Result : uint8_t {
    Ok = 0,
    InvalidPayload,
    BackendUnavailable,
    Failed,
    Forbidden,  // spec 17 §6: action gated off by permissions
};

// Spec 17 §6 autopilot permissions. The plugin/operator can lock out
// dangerous actions remotely. Defaults are conservative: engage is
// disabled, standby and heading adjust are allowed (so a user can
// safely return the AP to standby without having to enable engage).
struct Permissions {
    bool allow_engage = false;
    bool allow_standby = true;
    bool allow_heading_adjust = true;
};

struct State {
    Backend backend;
    Mode mode;
    double current_heading_rad = NAN;
    double target_heading_rad = NAN;
    double target_wind_angle_rad = NAN;
    bool alarm_active = false;
    bool warning_active = false;
    char alarm_text[48] = {0};
};

// Pure helpers - exposed for host tests. Live in autopilot_pure.cpp
// so the native env can link them without Arduino deps.
const char *mode_name(Mode m);
Mode mode_from_string(const char *s);
const char *backend_name(Backend b);
const char *result_name(Result r);

// True iff the given mode is allowed under the supplied permissions.
// Engage covers Auto / Wind / PreTrack / Track; Standby covers Standby;
// Unknown is never allowed. Pure - host-testable.
bool mode_allowed(Mode m, const Permissions &p);

// The rest of the API is Arduino-only (touches NVS, HTTPClient,
// the SK module). Host tests cover the pure helpers above.

#ifdef ARDUINO

Backend default_backend();
void set_default_backend(Backend b);
void copy_state(State &out);
Result set_mode(Mode m);
Result adjust_heading_deg(int delta);
Result silence_alarm();
void setup();

// Spec 17 §6 autopilot permissions - thread-safe at the set/get
// boundary; set_mode / adjust_heading_deg consult them before
// dispatching to a backend.
Permissions get_permissions();
void set_permissions(const Permissions &p);

// Console: autopilot | autopilot status | autopilot mode <m> |
//          autopilot heading <delta> | autopilot silence |
//          autopilot backend <signalk|nmea2000>
bool handleSerialCommand(const String &line);

#endif  // ARDUINO

}  // namespace autopilot
