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

// Push-live: request an immediate generated-config fetch (instead of waiting
// for the next command poll). Called when the manager plugin emits a
// configPush SignalK delta targeting this device. Thread-safe (sets a flag the
// manager worker drains).
void request_config_fetch();

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// --- Waveshare knob remote enumeration + switch (Phase F2) -----------------
// These reuse the manager worker's authenticated HTTP/JSON client. They block
// on HTTPClient, so they MUST be called only from the manager worker task
// (never the LVGL/UI task). Results are written into knob_remote under that
// module's registry lock.

// GET /devices/summary and refresh knob_remote's remote entries (1..N).
// Returns the number of remote displays ingested, or -1 on failure.
int knob_refresh_displays();

// GET /devices/:id/views for the remote display at knob_remote index `dev_idx`
// and fill its view list. Returns true on success.
bool knob_fetch_views(int dev_idx);

// POST /devices/:device_id/command with {type:"screen.set",payload:{screen}}.
// Returns true on a 2xx response.
bool knob_post_screen_set(const char *device_id, const char *screen_id);
#endif

}  // namespace manager
