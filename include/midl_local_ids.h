#pragma once

// Text-valued MIDL local ids ({kind:"local", id:"..."} bindings) — pure
// formatting half, host-tested. The device fills a Snapshot from its status
// APIs (midl_render_apply.cpp) and the painters render the returned text.
//
// Ids (docs/midl/midl-only.md): net.ip, net.ssid, net.rssi, sk.state, sk.host,
// sys.build, sys.version, sys.uptime, sys.heap, sys.psram, device.id, plus
// trip.underway ("H:MM:SS", the built-in trip record). The numeric trip ids
// (trip.distance nm, trip.avgSpeed / trip.maxSpeed kn) and the
// numeric legacy ids (temperature, heap, psram, rssi, uptime, optionally with
// a "device." prefix) stay on the numeric resolver. Unknown ids render "--".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>
#include <stdint.h>

namespace midl::local_ids {

struct Snapshot {
    const char *ip;        // "" when offline
    const char *ssid;      // "" when not associated
    int rssi;              // dBm; 0 = unknown
    const char *sk_state;  // sk::connectionStatus(): live/no-data/disconnected/...
    const char *sk_host;   // "host:port" or ""
    const char *build;     // build time / git commit
    const char *version;   // firmware version
    uint32_t uptime_s;
    uint32_t heap_free;   // bytes, internal
    uint32_t psram_free;  // bytes
    const char *device_id;
    uint32_t underway_s;  // trip log: seconds underway
};

// True when `id` is one of the text ids above (decides whether the painter
// asks the text resolver or the numeric one).
bool is_text_id(const char *id);

// Format `id` from `s` into out (always NUL-terminated). Unknown id, or a
// value the device does not have, -> "--". Returns false for unknown ids.
bool format(const char *id, const Snapshot &s, char *out, size_t cap);

// "3d 04:05", "04:05:06" (< 1 day). Exposed for tests.
void format_uptime(uint32_t s, char *out, size_t cap);
// "123 KB" (< 1 MB) / "7.5 MB". Exposed for tests.
void format_bytes(uint32_t b, char *out, size_t cap);

}  // namespace midl::local_ids
