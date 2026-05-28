#pragma once

// Recent-errors ring buffer for spec 17 §5 heartbeat diagnostics.
//
// Fixed capacity (no heap allocations after init) so it's safe to push
// from ISR-adjacent contexts too. Calls are mutex-free; the producer
// owns the writer index, and the consumer takes a stable snapshot.
// In the firmware the producer is net::logf when its sink saw an
// error-class line; in tests we push directly.
//
// Pure C++. The native test env links this verbatim.

#include <stddef.h>
#include <stdint.h>

namespace error_log {

constexpr size_t MAX_ENTRIES = 8;
constexpr size_t MAX_MESSAGE = 120;

struct Entry {
    uint32_t timestamp_ms;
    char message[MAX_MESSAGE];
};

// Push a new error onto the ring. Oldest entry is overwritten on
// overflow. Empty / null messages are ignored. timestamp_ms is supplied
// by the caller so the firmware can pass millis() and tests can pass
// a deterministic value.
void push(uint32_t timestamp_ms, const char *message);

// Number of entries currently stored (0..MAX_ENTRIES). After overflow
// this caps at MAX_ENTRIES.
size_t size();

// Copy up to `cap` entries into `out` in chronological order (oldest
// first). Returns how many were written.
size_t copy(Entry *out, size_t cap);

// Drop every stored entry. Test-only helper, but kept linked so the
// firmware can wire a `manager-errors clear` console verb later.
void clear();

}  // namespace error_log
