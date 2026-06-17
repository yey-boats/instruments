#pragma once

// Pure, host-testable set of SignalK path strings + a set-difference helper,
// used by the per-screen subscription manager (Slice 3 of the device-mirrored
// layout editor). The manager keeps three of these around:
//   - baseline   (always-on paths)
//   - desired    (baseline + active-screen paths)
//   - g_active   (currently subscribed)
// and diffs desired vs active to compute the subscribe / unsubscribe deltas so
// a path both screens need is never dropped and re-added.
//
// Fixed capacity, no heap churn. Mirrors path_store.h dimensions (CAP=64,
// PATH_LEN=80). A SubscriptionSet is ~5 KB; per CLAUDE.md "Memory traps" it
// must NEVER be declared on a task/callback stack - the live device instances
// are file-scope statics (single-task = race-free) or PSRAM-allocated.

#include <stddef.h>
#include <stdint.h>

namespace sk {

class SubscriptionSet {
  public:
    static constexpr int CAP = 64;       // mirror PathStore::CAP
    static constexpr int PATH_LEN = 80;  // mirror PathStore::PATH_LEN (longest SK path)

    void clear();

    // Insert `path`. No-op if already present (dedup). Returns false if `path`
    // is new and the set is full (existing members are kept). NULL/empty paths
    // are rejected (return false) and not stored. Paths >= PATH_LEN are
    // truncated to PATH_LEN-1 chars before storing.
    bool add(const char *path);

    bool has(const char *path) const;
    int size() const { return count_; }
    bool full() const { return count_ >= CAP; }

    // Stable iteration for the manager's subscribe/unsubscribe loops. Returns
    // the i-th stored path (0..size()-1), or nullptr if out of range.
    const char *at(int i) const;

  private:
    char paths_[CAP][PATH_LEN] = {};
    int count_ = 0;
    int find_(const char *path) const;  // index or -1
};

// Pure set difference. toAdd = desired - active (paths to subscribe);
// toRemove = active - desired (paths to unsubscribe). A path present in both
// desired and active appears in NEITHER output (kept subscribed, never
// dropped/re-added). toAdd and toRemove are cleared first. If either output
// overflows (more deltas than CAP) the surplus is silently dropped - sized so
// this never happens for the manifest caps in practice.
void diff(const SubscriptionSet &desired, const SubscriptionSet &active, SubscriptionSet &toAdd,
          SubscriptionSet &toRemove);

}  // namespace sk
