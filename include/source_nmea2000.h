#pragma once

// NMEA2000 listen-only source per docs/specs/12 Feature 2.
//
// On the 4848S040 there is no CAN transceiver. This file's task path
// is compiled in only when -DENABLE_NMEA2000 is set in build_flags AND
// when a transceiver is wired to the configured rx_pin/tx_pin. Without
// the flag, the module presents a no-op setup() + status() so callers
// (main.cpp, CLI dispatch) can include it unconditionally.
//
// When enabled, the worker:
//   - Opens the ESP32 TWAI driver in listen-only mode at 250 kbps.
//   - Reassembles fast/slow packets into PGNs.
//   - Decodes a small set of high-value PGNs and publishes their fields
//     into boat::Snapshot with SourceKind::Nmea2000.
//
// PGNs covered (first cut):
//   127250 Vessel heading
//   127508 Battery status
//   128267 Depth
//   129025 Position rapid
//   129026 COG/SOG rapid
//   130306 Wind data
//
// Decoding is implemented from public protocol references, not copied
// from any GPL source.

#include <Arduino.h>

namespace nmea2000 {

struct Status {
    bool compiled_in;  // true iff ENABLE_NMEA2000 was set at build
    bool enabled;      // runtime enable flag (NVS)
    bool sniff;        // verbose per-frame logging (spec 12 §4)
    bool tx_enabled;   // explicit gate before any future transmit
    int8_t rx_pin;
    int8_t tx_pin;
    uint32_t frames_rx;
    uint32_t pgns_decoded;
    uint32_t pgns_unknown;
    uint32_t last_rx_ms;
};

void setup();
Status status();

// Console: "n2k status | enable | disable | pins <rx> <tx>"
bool handleSerialCommand(const String &line);

}  // namespace nmea2000
