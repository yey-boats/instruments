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
    // Wall-clock millis when the WS transitioned to connected. Reset to 0
    // on disconnect. Used as the staleness reference until the first
    // delta arrives so a freshly-connected (or freshly-reconnected) link
    // doesn't immediately raise the "SIGNALK STALLED" alarm.
    uint32_t connectedSinceMs = 0;
    bool connected = false;
};

// Parse one SignalK delta message. Returns the number of `path/value`
// pairs that matched a known field and were applied to `out`.
//   = 0  ->  no relevant values (e.g. hello / keepalive)
//   < 0  ->  JSON parse error
int applyDelta(const char *json, size_t len, Data &out);

// Apply a single path/value pair. Public for unit testing.
void applyValue(const char *path, JsonVariant val, Data &out);

// Pure classifier for the SignalK link status string surfaced via
// /api/state.sk, the status screen, and the "SIGNALK STALLED" alarm.
//   "disconnected" -> WS not connected
//   "live"         -> WS connected and either fresh data within stallMs,
//                     or no data yet but still inside the warmup window
//                     measured from connectedSinceMs.
//   "stalled"      -> WS connected but no data within stallMs of the
//                     newer of (lastUpdateMs, connectedSinceMs).
const char *classifyStatus(bool connected, uint32_t lastUpdateMs,
                           uint32_t connectedSinceMs, uint32_t nowMs,
                           uint32_t stallMs = 10000);

}  // namespace sk
