#include "wifi_scan_json.h"

#include <stdio.h>
#include <string.h>

// Pure C++ — no Arduino/ESP includes (compiled into the native test TU).

namespace wifi_scan_json {

// Escape one SSID into `out` (cap includes NUL). Quotes and backslashes get
// a backslash prefix; control bytes (<0x20) are dropped rather than emitted
// as \uXXXX to keep entries compact.
static void escape_ssid(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (char)c;
    }
    out[o] = 0;
}

size_t to_json(const Ap *aps, size_t count, char *out, size_t cap) {
    if (!out || cap < 3) {
        if (out && cap) out[0] = 0;
        return 0;
    }
    if (count > MAX_APS) count = MAX_APS;

    // Order indices by RSSI descending (selection sort; count is tiny).
    size_t idx[MAX_APS];
    for (size_t i = 0; i < count; ++i)
        idx[i] = i;
    for (size_t i = 0; i + 1 < count; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < count; ++j) {
            if (aps[idx[j]].rssi > aps[idx[best]].rssi) best = j;
        }
        size_t t = idx[i];
        idx[i] = idx[best];
        idx[best] = t;
    }

    size_t pos = 0;
    out[pos++] = '[';
    bool first = true;
    for (size_t i = 0; i < count && aps; ++i) {
        const Ap &ap = aps[idx[i]];
        if (!ap.ssid[0]) continue;
        char esc[70];
        escape_ssid(ap.ssid, esc, sizeof(esc));
        char entry[112];
        int el = snprintf(entry, sizeof(entry), "{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%s}", esc,
                          (int)ap.rssi, ap.sec ? "true" : "false");
        if (el <= 0 || (size_t)el >= sizeof(entry)) continue;
        size_t need = (size_t)el + (first ? 0 : 1);
        // Must still fit the closing ']' and the NUL after this entry.
        if (pos + need + 2 > cap) break;
        if (!first) out[pos++] = ',';
        memcpy(out + pos, entry, (size_t)el);
        pos += (size_t)el;
        first = false;
    }
    out[pos++] = ']';
    out[pos] = 0;
    return pos;
}

}  // namespace wifi_scan_json
