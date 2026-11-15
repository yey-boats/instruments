// Text-valued MIDL local ids. See include/midl_local_ids.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_local_ids.h"

#include <stdio.h>
#include <string.h>

namespace midl::local_ids {

namespace {

const char *const TEXT_IDS[] = {"net.ip",    "net.ssid",      "net.rssi",   "sk.state", "sk.host",
                                "sys.build", "sys.version",   "sys.uptime", "sys.heap", "sys.psram",
                                "device.id", "trip.underway", nullptr};

void put(char *out, size_t cap, const char *s) {
    if (!cap) return;
    snprintf(out, cap, "%s", (s && s[0]) ? s : "--");
}

}  // namespace

bool is_text_id(const char *id) {
    if (!id) return false;
    for (const char *const *p = TEXT_IDS; *p; ++p)
        if (strcmp(*p, id) == 0) return true;
    return false;
}

void format_uptime(uint32_t s, char *out, size_t cap) {
    uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, sec = s % 60;
    if (d)
        snprintf(out, cap, "%ud %02u:%02u", (unsigned)d, (unsigned)h, (unsigned)m);
    else
        snprintf(out, cap, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)sec);
}

void format_bytes(uint32_t b, char *out, size_t cap) {
    if (b < 1024u * 1024u)
        snprintf(out, cap, "%u KB", (unsigned)(b / 1024u));
    else
        snprintf(out, cap, "%.1f MB", (double)b / (1024.0 * 1024.0));
}

bool format(const char *id, const Snapshot &s, char *out, size_t cap) {
    if (!out || !cap) return false;
    if (!is_text_id(id)) {
        put(out, cap, nullptr);
        return false;
    }
    if (!strcmp(id, "net.ip")) {
        put(out, cap, s.ip);
    } else if (!strcmp(id, "net.ssid")) {
        put(out, cap, s.ssid);
    } else if (!strcmp(id, "net.rssi")) {
        if (s.rssi == 0)
            put(out, cap, nullptr);
        else
            snprintf(out, cap, "%d dBm", s.rssi);
    } else if (!strcmp(id, "sk.state")) {
        put(out, cap, s.sk_state);
    } else if (!strcmp(id, "sk.host")) {
        put(out, cap, s.sk_host);
    } else if (!strcmp(id, "sys.build")) {
        put(out, cap, s.build);
    } else if (!strcmp(id, "sys.version")) {
        put(out, cap, s.version);
    } else if (!strcmp(id, "sys.uptime")) {
        format_uptime(s.uptime_s, out, cap);
    } else if (!strcmp(id, "sys.heap")) {
        format_bytes(s.heap_free, out, cap);
    } else if (!strcmp(id, "sys.psram")) {
        format_bytes(s.psram_free, out, cap);
    } else if (!strcmp(id, "device.id")) {
        put(out, cap, s.device_id);
    } else if (!strcmp(id, "trip.underway")) {
        snprintf(out, cap, "%u:%02u:%02u", (unsigned)(s.underway_s / 3600),
                 (unsigned)((s.underway_s / 60) % 60), (unsigned)(s.underway_s % 60));
    }
    return true;
}

}  // namespace midl::local_ids
