#pragma once

// Management plane client per docs/specs/17 F2.
//
// Talks to the SignalK ESP Display Manager plugin (spec 18) over HTTP:
//   POST /devices/register  - obtain a device token
//   POST /devices/:id/status - 30 s heartbeat
//   GET  /devices/:id/config - apply central config
//   GET  /devices/:id/commands - poll and ack queued commands
//
// Lives on its own FreeRTOS task pinned to core 0 so blocking HTTPClient
// connects can't stall LVGL. NVS-persistent endpoint + bearer token
// under the "mgr" namespace.

#include <Arduino.h>
#include <stdint.h>

namespace manager {

enum class AuthState : uint8_t {
    Unprovisioned = 0,
    Provisioned,
};

enum class HealthState : uint8_t {
    Idle = 0,  // no endpoint configured
    Registering,
    Heartbeating,
    Failed,
};

struct Status {
    AuthState auth;
    HealthState health;
    String endpoint;  // "http://host:port" or ""
    String endpoint_host;
    String endpoint_base_path;
    uint16_t endpoint_port;
    bool endpoint_tls;
    String discovery_method;
    bool has_token;
    bool has_sk_token;  // server-issued SK bearer for auth header
    uint32_t last_register_ms;
    int last_register_code;  // HTTP status, negative on transport error
    uint32_t last_heartbeat_ms;
    int last_heartbeat_code;
    // S5: the heartbeat code alone is ambiguous (pre-flight refusals shared
    // negative codes). last_heartbeat_status is the ManagerStatus category
    // name ("Ok"/"WifiDown"/"LowHeap"/"NotProvisioned"/"SendFailed"/...).
    // The two counters separate pre-flight refusals (we declined to send:
    // not provisioned / low heap / WiFi down) from transport failures (we
    // tried and the network/server burned it) so a heartbeat that loops at
    // a non-2xx code can be classified at a glance.
    String last_heartbeat_status;
    uint32_t heartbeat_preflight_refusals;
    uint32_t heartbeat_transport_failures;
    uint32_t heartbeat_interval_ms;
    uint32_t command_poll_interval_ms;
    String device_id;
    String config_version;
    String config_hash;
    // Spec 17 §11 command diagnostics
    uint8_t pending_cmd_count;
    uint32_t last_cmd_ms;
    String last_cmd_id;
    String last_cmd_type;
    String last_cmd_result;
    // Spec 17 §11 firmware-update state
    bool ota_in_flight;
    bool ota_confirm_pending;
    String ota_job_id;
};

void setup();

// CLI: manager-register <url> | manager-status | manager-token <jwt> |
//      manager-forget | manager-discover (F2 mdns)
bool handleSerialCommand(const String &line);

Status status();

// Returns true iff the device has a saved endpoint AND a token.
bool is_provisioned();

}  // namespace manager
