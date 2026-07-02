#pragma once

// Pure (host-testable) serializer for WiFi scan results going out over the
// BLE WIFI-SCAN characteristic (a3f7e004-...). BLE attribute values are
// capped at 512 bytes, so the output is sorted strongest-first and weaker
// APs are dropped once the cap would be exceeded — the result is always a
// complete, valid JSON array. No Arduino dependencies.

#include <stddef.h>
#include <stdint.h>

namespace wifi_scan_json {

constexpr size_t MAX_APS = 24;  // more than fits in 512 B anyway

struct Ap {
    char ssid[33];  // NUL-terminated, up to 32 bytes of SSID
    int16_t rssi;
    bool sec;  // true = any encryption, false = open
};

// Serialize up to `count` APs into `out` (capacity `cap`, including the NUL)
// as: [{"ssid":"...","rssi":-42,"sec":true}, ...]
// - sorted by RSSI descending (strongest first)
// - SSIDs are JSON-escaped (quotes/backslashes); control bytes are dropped
// - entries that would overflow `cap` are omitted (truncate-to-strongest)
// Returns the string length written (0 on cap < 3 or null out).
size_t to_json(const Ap *aps, size_t count, char *out, size_t cap);

}  // namespace wifi_scan_json
