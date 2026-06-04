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
    // Wall-clock millis on the last received WS TEXT frame, regardless of
    // whether applyDelta matched any value-bearing path. This is the WS
    // *link* freshness signal as opposed to *data* freshness: SK servers
    // legitimately send hello, subscription acks, and envelope-only
    // deltas (no `values`), which keep the pipe alive but wouldn't tick
    // lastUpdateMs. Counting them here prevents the alarm from firing
    // during normal idle gaps when the link is healthy.
    uint32_t wsLastFrameMs = 0;
    bool connected = false;
};

// Parse one SignalK delta message. Returns the number of `path/value`
// pairs that matched a known field and were applied to `out`.
//   = 0  ->  no relevant values (e.g. hello / keepalive)
//   < 0  ->  JSON parse error
// Pass `alloc` (e.g. &espdisp::psram_json) to keep the parser's working
// buffer off internal heap on the device; nullptr (default) uses the
// internal-heap allocator, which host tests rely on.
int applyDelta(const char *json, size_t len, Data &out, ArduinoJson::Allocator *alloc = nullptr);

// Apply a single path/value pair. Public for unit testing.
void applyValue(const char *path, JsonVariant val, Data &out);

// Pure classifier for the SignalK link status string surfaced via
// /api/state.sk, the status screen, and the "SIGNALK STALLED" alarm.
//
// Returns:
//   "disconnected" -> WS not connected.
//   "live"         -> WS connected and at least one value-bearing delta
//                     has been received (lastUpdateMs != 0), with link
//                     activity within stallMs.
//   "no-data"      -> WS connected and frames flowing, but no
//                     value-bearing delta yet AND we've been connected
//                     longer than noDataMs (server has no producers).
//   "stalled"      -> WS connected but no WS activity at all within
//                     stallMs of nowMs.
//
// All three timestamps are uint32_t millis(). The reference for the
// staleness check is whichever of (lastUpdateMs, wsLastFrameMs,
// connectedSinceMs) is most recent in modular-arithmetic terms, so
// hello/ack/envelope-only frames (which don't tick lastUpdateMs but
// do tick wsLastFrameMs) are correctly treated as link activity.
//
// stallMs default of 30000 matches the WebSocketsClient heartbeat
// horizon (15s ping x 2 retries + 3s pong wait ~= 33s). Shorter
// thresholds reliably alarm before the WS library has even decided
// whether the link is alive.
//
// noDataMs default of 10000 gives a fresh connection a 10s warmup
// before being downgraded from "live" to "no-data".
const char *classifyStatus(bool connected, uint32_t lastUpdateMs, uint32_t connectedSinceMs,
                           uint32_t wsLastFrameMs, uint32_t nowMs, uint32_t stallMs = 30000,
                           uint32_t noDataMs = 10000);

}  // namespace sk
