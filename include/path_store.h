#pragma once

// Generic dynamic SignalK path -> latest-value store. Lets the renderer
// resolve ANY configured path string (not just the curated MetricSource
// set) so authored layout fields render without a firmware recompile.
//
// Fixed capacity (no heap churn): one entry per (view * tile * named-path)
// the device can show at once, bounded by the capability manifest caps.
// Pure C++ / host-testable; the live device instance is PSRAM-allocated.

#include <math.h>
#include <stddef.h>

namespace sk {

class PathStore {
  public:
    static constexpr int CAP = 64;       // >= maxViews(8) * maxTiles(4) * 2 named paths
    static constexpr int PATH_LEN = 80;  // longest SignalK path we accept

    void clear();

    // Upsert path -> value. Returns false if `path` is new and the store is
    // full (existing paths always update). NaN values are stored (a path that
    // went invalid reads back NaN, distinct from "absent").
    bool set(const char *path, double value);

    // Latest value for `path`, or NaN if the path was never stored.
    double get(const char *path) const;

    bool has(const char *path) const;
    int size() const { return count_; }

  private:
    struct Entry {
        char path[PATH_LEN];
        double value;
        bool used;
    };
    Entry entries_[CAP] = {};
    int count_ = 0;
    int find_(const char *path) const;  // index or -1
};

}  // namespace sk
