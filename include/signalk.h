#pragma once

// Top-level SignalK module: WebSocket client, NVS-persisted target,
// console commands. Pulls in the pure parser (signalk_parser.h) which
// owns the Data struct.

#include <Arduino.h>
#include "signalk_parser.h"

namespace sk {

extern Data data;

void setup(const String &host, uint16_t port);
void loop();

// Set host/port at runtime (via 'sk <host> [port]' on serial/BLE),
// persisted to NVS. Returns true if the line was consumed.
bool handleSerialCommand(const String &line);

String connectionStatus();

}  // namespace sk
