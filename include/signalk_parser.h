#pragma once

// Pure SignalK delta parser. No Arduino dependencies - compiles for both
// the ESP32 firmware target and the native host (for unit tests).

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(NAN)
#include <math.h>
#endif

#include "ais_store.h"
#include "boat_data.h"
#include "notifications.h"
#include "path_store.h"

namespace sk {

// The parser fills a boat::View (the source-neutral flat render struct,
// defined in boat_data.h). It only touches the metric fields; the WS
// link-state fields on View are owned by signalk.cpp.

// Optional side-channel sinks for delta content that is NOT self-vessel
// metric data (phase 5 data layer). Both stores are header-inline pure C++
// (notifications.h / ais_store.h) so this stays host-testable.
//
// Context routing: a delta whose top-level "context" is absent,
// "vessels.self", or equal to `self_id` is SELF and fills boat::View as
// before. A context naming ANOTHER vessel ("vessels.urn:mrn:imo:mmsi:<n>")
// never touches View: its navigation.position / speedOverGround /
// courseOverGroundTrue (+ name, when a static-tree delta carries one) are
// routed into `ais` instead. Non-self vessels without an MMSI-style urn
// (uuid contexts) are dropped. notifications.* paths on SELF are routed
// into `notifications` ({state,message,method[]} object values; a "normal"
// state clears the entry).
struct DeltaSinks {
    notif::Store *notifications = nullptr;  // nullptr = drop notifications.*
    ais::Store *ais = nullptr;              // nullptr = drop non-self vessels
    // Self context as reported by the server hello (e.g.
    // "vessels.urn:mrn:imo:mmsi:239123456"; matched with or without the
    // "vessels." prefix on either side). nullptr = only literal
    // "vessels.self" / absent context count as self.
    const char *self_id = nullptr;
    uint32_t now_ms = 0;  // timestamp stamped onto store upserts
};

// Parse one SignalK delta message. Returns the number of `path/value`
// pairs that matched a known field and were applied to `out` (self metric
// fields, notification upserts, and AIS target updates all count - the
// return feeds link liveness, not the touched mask).
//   = 0  ->  no relevant values (e.g. hello / keepalive)
//   < 0  ->  JSON parse error
// Pass `alloc` (e.g. &yeyboats::psram_json) to keep the parser's working
// buffer off internal heap on the device; nullptr (default) uses the
// internal-heap allocator, which host tests rely on.
// `dyn` (optional): mirror every numeric SELF delta value into this dynamic
// store so authored fields can render arbitrary paths by string. nullptr to skip.
// `touched` (optional): OR-accumulates a boat::field_bit() for every View
// field THIS delta actually carried a usable value for. The SignalK ingest
// path republishes only those fields, so per-field staleness survives a
// chatty link (see boat::ingest_signalk). Not cleared here - caller inits.
// `sinks` (optional): see DeltaSinks. nullptr = legacy self-only behavior.
int applyDelta(const char *json, size_t len, boat::View &out,
               ArduinoJson::Allocator *alloc = nullptr, PathStore *dyn = nullptr,
               boat::FieldMask *touched = nullptr, const DeltaSinks *sinks = nullptr);

// Extract the "self" identity from a SignalK server hello frame
// ({"self":"vessels.urn:...","version":...}) into `out` (NUL-terminated,
// truncating). Returns true when the frame carried a self field. The WS
// layer calls this on frames containing "self" and passes the captured id
// as DeltaSinks::self_id so vessels.* deltas about our own ship keep
// routing to boat::View rather than the AIS store.
bool parseHello(const char *json, size_t len, char *out, size_t cap,
                ArduinoJson::Allocator *alloc = nullptr);

// Apply a single path/value pair. Public for unit testing. `touched` as in
// applyDelta: bits are set only for fields this value actually populated.
void applyValue(const char *path, JsonVariant val, boat::View &out,
                boat::FieldMask *touched = nullptr);

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
