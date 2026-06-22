#pragma once

// Pure SignalK delta parser. No Arduino dependencies - compiles for both
// the ESP32 firmware target and the native host (for unit tests).

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(NAN)
#include <math.h>
#endif

#include "boat_data.h"
#include "path_store.h"

namespace sk {

// The parser fills a boat::View (the source-neutral flat render struct,
// defined in boat_data.h). It only touches the metric fields; the WS
// link-state fields on View are owned by signalk.cpp.

// Parse one SignalK delta message. Returns the number of `path/value`
// pairs that matched a known field and were applied to `out`.
//   = 0  ->  no relevant values (e.g. hello / keepalive)
//   < 0  ->  JSON parse error
// Pass `alloc` (e.g. &yeyboats::psram_json) to keep the parser's working
// buffer off internal heap on the device; nullptr (default) uses the
// internal-heap allocator, which host tests rely on.
// `dyn` (optional): mirror every numeric delta value into this dynamic store
// so authored fields can render arbitrary paths by string. nullptr to skip.
int applyDelta(const char *json, size_t len, boat::View &out,
               ArduinoJson::Allocator *alloc = nullptr, PathStore *dyn = nullptr);

// Apply a single path/value pair. Public for unit testing.
void applyValue(const char *path, JsonVariant val, boat::View &out);

#ifdef DBG_PERF_COUNTERS
// Benchmark: total path/value pairs seen by applyDelta since the last call
// (read-and-reset). Counts every pair, matched or not (throughput, not just
// the value-bearing matches applyDelta returns).
uint32_t takeParsedCount();
#endif

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
