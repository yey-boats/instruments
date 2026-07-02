#pragma once

// BLE GATT services exposing characteristics that a smartphone app or a
// peer controller can read/write:
//
//   CONNECTION    - JSON snapshot of network + SignalK state and config
//   CONFIGURATION - the layout JSON document (same schema as SignalK
//                   resource at configuration.boat-mfd.layouts)
//   WIFISCAN      - write "scan" to trigger an async WiFi scan; read/notify
//                   a JSON array of {ssid,rssi,sec} (strongest first, <=512 B)
//
//   Control GATT service (espdisp control protocol, BLE fallback transport):
//   DEVICE  - read   serialized proto::DeviceRecord (views capped to fit 512 B)
//   CONTROL - write  one protocol message {attach|switch|heartbeat|detach}
//   STATE   - read+notify serialized proto::ControlState (active sessions)
//   RESP    - read+notify last ack JSON (lets a central read back sessionId,
//             since a BLE WRITE has no response payload)
//
// Service / characteristic UUIDs are stable and may be relied on by
// companion apps + controllers. See README "BLE GATT schema" for the JSON
// shapes; the Control service mirrors the HTTP /api/p2p/* path exactly and
// reuses the same proto_target handlers.

#include <Arduino.h>

namespace bleconfig {

// clang-format off
// Configuration service (CONNECTION + CONFIGURATION + WIFISCAN).
constexpr const char *SERVICE_UUID   = "a3f7e000-7a6b-4f47-b3a5-c4d2e5f6a000";
constexpr const char *CONN_UUID      = "a3f7e001-7a6b-4f47-b3a5-c4d2e5f6a000";
constexpr const char *CONFIG_UUID    = "a3f7e003-7a6b-4f47-b3a5-c4d2e5f6a000";
// WIFISCAN (BLE-3): write the ASCII token "scan" to kick an async WiFi scan;
// read/notify returns a JSON array of {ssid,rssi,sec} sorted strongest-first
// and truncated to the 512-byte attribute cap. While a scan runs the value
// reads {"running":true}.
constexpr const char *WIFISCAN_UUID  = "a3f7e004-7a6b-4f47-b3a5-c4d2e5f6a000";

// Control service (espdisp control protocol over BLE).
constexpr const char *CTRL_SERVICE_UUID = "a3f7e100-7a6b-4f47-b3a5-c4d2e5f6a000";
constexpr const char *CTRL_DEVICE_UUID  = "a3f7e101-7a6b-4f47-b3a5-c4d2e5f6a000";  // READ
constexpr const char *CTRL_CONTROL_UUID = "a3f7e102-7a6b-4f47-b3a5-c4d2e5f6a000";  // WRITE
constexpr const char *CTRL_STATE_UUID   = "a3f7e103-7a6b-4f47-b3a5-c4d2e5f6a000";  // READ + NOTIFY
constexpr const char *CTRL_RESP_UUID    = "a3f7e104-7a6b-4f47-b3a5-c4d2e5f6a000";  // READ + NOTIFY
// clang-format on

// Wire the service into an already-created NimBLEServer. Idempotent.
void setup();

// Refresh the values backing the characteristics and notify any
// subscribed clients. Safe to call frequently.
void notifyAll();

}  // namespace bleconfig
