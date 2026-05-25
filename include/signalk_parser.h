#pragma once

// Pure SignalK delta parser. No Arduino dependencies - compiles for both
// the ESP32 firmware target and the native host (for unit tests).

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(NAN)
#include <math.h>
#endif

namespace sk {

struct Data {
    // navigation
    double lat = NAN, lon = NAN;
    double sog = NAN;          // speed over ground, m/s
    double stw = NAN;          // speed through water, m/s
    double cogTrue = NAN;      // course over ground (true), rad
    double headingTrue = NAN;  // true heading, rad

    // environment
    double awa = NAN;        // apparent wind angle, rad
    double aws = NAN;        // apparent wind speed, m/s
    double twa = NAN;        // true wind angle, rad
    double tws = NAN;        // true wind speed, m/s
    double depth = NAN;      // m below transducer
    double waterTemp = NAN;  // K

    // electrical & tanks
    double battVoltage = NAN;  // V
    double battSoc = NAN;      // 0..1
    double tankFuel = NAN;     // 0..1
    double tankWater = NAN;    // 0..1

    // routing / steering (when a route is active on the SignalK server)
    double xte = NAN;          // cross-track error, m (signed: + = right of track)
    double cts = NAN;          // course to steer, rad
    double btw = NAN;          // bearing to waypoint, rad
    double dtw = NAN;          // distance to waypoint, m
    double vmg = NAN;          // velocity made good, m/s
    double apTargetHdg = NAN;  // autopilot target heading, rad
    char apState[16] = {0};    // autopilot state string ("auto", "wind", "standby", ...)

    // current / tide
    double currentSetTrue = NAN;  // true direction the current is flowing toward, rad
    double currentDrift = NAN;    // current speed, m/s

    uint32_t lastUpdateMs = 0;
    bool connected = false;
};

// Parse one SignalK delta message. Returns the number of `path/value`
// pairs that matched a known field and were applied to `out`.
//   = 0  ->  no relevant values (e.g. hello / keepalive)
//   < 0  ->  JSON parse error
int applyDelta(const char *json, size_t len, Data &out);

// Apply a single path/value pair. Public for unit testing.
void applyValue(const char *path, JsonVariant val, Data &out);

}  // namespace sk
