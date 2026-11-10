#pragma once
// Controller-side mDNS discovery of espdisp displays. Browses `_espdisp._tcp`
// via the ESP-IDF `mdns` component and reports each peer's identity + IP + the
// protocol-version/role TXT keys, so a controller (the knob) can build an IP
// transport to it with no manager in the loop (design §4.1).
//
// The browse is BLOCKING (it waits up to `timeout_ms` for responses) and so
// MUST run on a worker/network task, never on the LVGL UI task. It mirrors the
// manager's device-list refresh cadence.
#include <Arduino.h>

namespace proto_discovery {

// One discovered espdisp peer. Strings are NUL-terminated, sized to the same
// bounds the registry uses. `base_url` is "http://<ip>:<port>" ready for
// proto_ip::* calls.
struct Peer {
    char device_id[40] = {0};
    char board[24] = {0};
    char display[16] = {0};
    char pv[8] = {0};     // protocol version TXT (e.g. "2.0"); empty if absent
    char role[12] = {0};  // "display" | "controller" | "both"; empty if absent
    char ip[16] = {0};    // dotted IPv4
    uint16_t port = 0;
    char base_url[40] = {0};
};

// Callback invoked once per discovered peer, on the calling (worker) task.
typedef void (*PeerCallback)(const Peer &peer, void *ctx);

// Browse `_espdisp._tcp` and invoke `cb` for each peer found within
// `timeout_ms` (capped at `max_results`). Returns the number of peers reported,
// or -1 if the mDNS query failed. BLOCKING — call on the worker task only.
int browse(PeerCallback cb, void *ctx, uint32_t timeout_ms = 1200, size_t max_results = 16);

}  // namespace proto_discovery
