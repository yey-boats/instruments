#pragma once

// Generic dynamic SignalK path -> latest-value store. Lets the renderer
// resolve ANY configured path string (not just the curated MetricSource
// set) so authored layout fields render without a firmware recompile.
//
// Fixed capacity (no heap churn): one entry per (view * tile * named-path)
// the device can show at once, bounded by the capability manifest caps.
// Lookup is an open-addressed hash (FNV-1a over the path, linear probe)
// instead of a linear strncmp scan - set() runs on the SignalK parse hot
// path for EVERY numeric delta value, and O(CAP) string scans there were
// measurable. Entries stay dense in insertion order so iteration via
// size()/path_at()/value_at() remains possible.
//
// Pure C++ / host-testable; the live device instance is PSRAM-allocated.
// On the device the store does its own locking (SK task writes, UI task
// reads) so callers never need an external mutex.

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace sk {

class PathStore {
  public:
    static constexpr int CAP = 160;      // >= maxViews(8) * maxTiles(9) * 2 named paths (=144)
    static constexpr int PATH_LEN = 80;  // longest SignalK path we accept
    // Hash slot count. Power of two (mask probing) and > CAP so the linear
    // probe always terminates on an empty slot (max load factor 160/256).
    static constexpr int SLOTS = 256;

    PathStore();

    void clear();

    // Upsert path -> value. Returns false if `path` is new and the store is
    // full (existing paths always update). NaN values are stored (a path that
    // went invalid reads back NaN, distinct from "absent").
    bool set(const char *path, double value);

    // Latest value for `path`, or NaN if the path was never stored.
    double get(const char *path) const;

    bool has(const char *path) const;
    int size() const { return count_; }

    // Dense iteration over stored entries in insertion order, i in
    // [0, size()). Out-of-range i returns nullptr / NaN.
    const char *path_at(int i) const;
    double value_at(int i) const;

#ifdef DBG_PERF_COUNTERS
    // Monotonic lookup counter (get/has), for the benchmark harness. Read and
    // reset with takeLookups(); compiled out unless DBG_PERF_COUNTERS.
    uint32_t takeLookups() {
        uint32_t v = lookups_;
        lookups_ = 0;
        return v;
    }
#endif

  private:
    struct Entry {
        char path[PATH_LEN];
        double value;
    };
    Entry entries_[CAP] = {};  // dense, insertion-ordered [0, count_)
    int16_t slots_[SLOTS];     // hash slot -> entries_ index, -1 = empty
    int count_ = 0;
#ifdef DBG_PERF_COUNTERS
    mutable uint32_t lookups_ = 0;
#endif
#ifdef ARDUINO
    // Guards entries_/slots_/count_: the SK parse task upserts while the UI
    // task resolves authored fields. Short critical sections (one probe or
    // one strncpy). Host builds are single-threaded per test - no lock.
    SemaphoreHandle_t mtx_ = nullptr;
#endif
    void lock_() const;
    void unlock_() const;
    int find_(const char *path) const;  // entries_ index or -1 (caller holds lock)
};

}  // namespace sk
