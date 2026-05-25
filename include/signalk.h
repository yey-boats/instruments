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

// PUT a value to a SignalK path on the configured server. Body is
// {"value": <serialized>}. value is sent as-is (caller must format e.g.
// "1.234" for numbers, "\"auto\"" for strings). Returns the HTTP status,
// or a negative value on transport error.
int putValue(const char *path, const char *valueJson);

// Thread-safe snapshot copy of sk::data. Use this from any task that
// isn't the SK parser task (UI, web, BLE). Cheap; copies under a short
// critical section.
void copyData(Data &out);

}  // namespace sk
