#include "error_log.h"

#include <string.h>

namespace error_log {

namespace {

Entry s_entries[MAX_ENTRIES] = {};
size_t s_write = 0;  // index of the next slot to overwrite
size_t s_count = 0;  // saturating count (capped at MAX_ENTRIES)

}  // namespace

void push(uint32_t timestamp_ms, const char *message) {
    if (!message || !*message) return;
    Entry &e = s_entries[s_write];
    e.timestamp_ms = timestamp_ms;
    strncpy(e.message, message, sizeof(e.message) - 1);
    e.message[sizeof(e.message) - 1] = '\0';
    s_write = (s_write + 1) % MAX_ENTRIES;
    if (s_count < MAX_ENTRIES) s_count++;
}

size_t size() {
    return s_count;
}

size_t copy(Entry *out, size_t cap) {
    if (!out || cap == 0 || s_count == 0) return 0;
    size_t n = s_count < cap ? s_count : cap;
    // Oldest entry: when full, it's at s_write (the next-overwrite
    // slot); when partial, it's at index 0.
    size_t start = s_count < MAX_ENTRIES ? 0 : s_write;
    for (size_t i = 0; i < n; ++i) {
        out[i] = s_entries[(start + i) % MAX_ENTRIES];
    }
    return n;
}

void clear() {
    s_write = 0;
    s_count = 0;
    for (size_t i = 0; i < MAX_ENTRIES; ++i) {
        s_entries[i].timestamp_ms = 0;
        s_entries[i].message[0] = '\0';
    }
}

}  // namespace error_log
