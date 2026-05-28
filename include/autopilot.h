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
//
// Safety rules from spec 12:
//   * Commands never run from LVGL callbacks; this module's writes
//     post to the net worker queue.
//   * Every command has visible queued/pending/result state.
//   * Track / wind / tack and alarm-silence actions require explicit
//     controls (the API distinguishes them by name; callers must not
//     wire them to a free-form swipe).

#include <Arduino.h>
#include <math.h>
#include <stdint.h>

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

// Pure helpers - exposed for host tests.
const char *mode_name(Mode m);
Mode mode_from_string(const char *s);
const char *backend_name(Backend b);

// Current default backend.
Backend default_backend();
void set_default_backend(Backend b);

// Snapshot the current state from the active backend.
void copy_state(State &out);

// Set the autopilot mode (standby / auto / wind / track / ...).
Result set_mode(Mode m);

// Adjust the locked heading by `delta` degrees. Positive = right.
// On the SignalK backend this maps to actions/adjustHeading.
Result adjust_heading_deg(int delta);

// Silence the currently sounding alarm. On SK this PUTs to
// notifications.silence (or a backend-specific path); on N2k this
// sends the corresponding PGN.
Result silence_alarm();

// One-time module init. Called from main.cpp setup() AFTER sk::setup.
void setup();

// Console: autopilot | autopilot status | autopilot mode <m> |
//          autopilot heading <delta> | autopilot silence |
//          autopilot backend <signalk|nmea2000>
bool handleSerialCommand(const String &line);

}  // namespace autopilot
