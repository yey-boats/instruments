#pragma once

// Top-level SignalK module: WebSocket client, NVS-persisted target,
// console commands. Pulls in the pure parser (signalk_parser.h) which
// owns the Data struct.

#include <Arduino.h>
#include "path_store.h"
#include "signalk_parser.h"

namespace sk {

extern Data data;

// Process-wide dynamic path store, fed from every numeric WS delta. The
// renderer resolves authored-field paths against this when they are not a
// typed sk::Data field. PSRAM-allocated on first use (see signalk.cpp).
PathStore &dynamicStore();

#ifdef DBG_PERF_COUNTERS
// Benchmark snapshot of the SignalK/throughput counters. Rates are per the
// interval since the previous benchNetTake() call (read-and-reset).
struct BenchNet {
    uint32_t ws_frames;       // WS TEXT frames received
    uint32_t ws_bytes;        // total bytes of those frames
    uint32_t deltas;          // value-bearing path/value pairs applied
    uint32_t parsed;          // total path/value pairs seen (matched or not)
    uint32_t parse_us_total;  // sum of applyDelta() durations
    uint32_t parse_us_peak;   // worst single applyDelta() duration
    uint32_t store_lookups;   // PathStore get/has calls
    int store_size;           // distinct dynamic paths currently held
    int subscriptions;        // active subscribed path count
};
BenchNet benchNetTake();
#endif  // DBG_PERF_COUNTERS

void setup(const String &host, uint16_t port);
void loop();

// Set host/port at runtime (via 'sk <host> [port]' on serial/BLE),
// persisted to NVS. Returns true if the line was consumed.
bool handleSerialCommand(const String &line);

String connectionStatus();

// PUT a value to a SignalK path on the configured server. Body is
// {"value": <serialized>}. value is sent as-is (caller must format e.g.
// "1.234" for numbers, "\"auto\"" for strings). Returns the HTTP status,
// or a negative value on transport error.
int putValue(const char *path, const char *valueJson);

// Thread-safe snapshot copy of sk::data. Use this from any task that
// isn't the SK parser task (UI, web, BLE). Cheap; copies under a short
// critical section.
void copyData(Data &out);

// Diagnostics: how many iterations the SK task has completed (ever-
// growing), and the longest single ws.loop() call since the last read
// (reading clears the peak so each /api/state sample is a fresh window).
uint32_t loopIters();
uint32_t loopMaxUs();

// Forensic stall telemetry. Call this once per UI tick (alongside the
// alarm_check that drives the "SIGNALK STALLED" banner). On each
// transition into or out of the stalled state, emits one net::logf line
// capturing all four staleness signals (last-update age, last-frame
// age, connect-time age, WS connected bit), the sk_task iteration delta
// and peak ws.loop() duration since the previous transition, and the
// WiFi state + RSSI at that instant. Cheap on no-transition (one mutex
// snapshot, a few comparisons). Output lands in the UDP log stream
// (port 9999) and BLE NUS notifications, so a stall recorded in the
// field leaves a forensic trail without needing the device live.
void pollStallTelemetry();

// mDNS auto-discovery. When no manual host has been set, the SK task
// periodically queries _signalk-ws._tcp.local. and uses the first
// record. `manual` mode disables discovery so the saved host wins.
//   `sk-discover`     - one-shot discovery, log result
//   `sk-host auto`    - clear manual host, enable discovery
//   `sk-host manual <host> [port]` - pin to this host (no discovery)
bool isAutoMode();
bool tryAutoDiscover(uint32_t now_ms);  // safe to call from sk_task; returns true if a host was set
String targetHost();
uint16_t targetPort();

}  // namespace sk
