#include "path_store.h"

#include <string.h>

namespace sk {

// FNV-1a over at most PATH_LEN-1 chars (the stored-key length, so a
// too-long path hashes the same as its truncated stored form).
static uint32_t fnv1a_bounded(const char *s) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < PathStore::PATH_LEN - 1 && s[i]; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

PathStore::PathStore() {
#ifdef ARDUINO
    mtx_ = xSemaphoreCreateMutex();
#endif
    for (int i = 0; i < SLOTS; ++i)
        slots_[i] = -1;
    count_ = 0;
}

void PathStore::lock_() const {
#ifdef ARDUINO
    if (mtx_) xSemaphoreTake(mtx_, portMAX_DELAY);
#endif
}

void PathStore::unlock_() const {
#ifdef ARDUINO
    if (mtx_) xSemaphoreGive(mtx_);
#endif
}

// Probe for `path`'s entry index, -1 if absent. Caller holds the lock.
// Linear probe from the FNV-1a slot; slots_ has more slots than entries can
// ever exist (SLOTS > CAP) and we never delete, so an empty slot ends every
// unsuccessful probe.
int PathStore::find_(const char *path) const {
    if (!path) return -1;
    uint32_t h = fnv1a_bounded(path);
    for (int step = 0; step < SLOTS; ++step) {
        int slot = (int)((h + (uint32_t)step) & (SLOTS - 1));
        int16_t idx = slots_[slot];
        if (idx < 0) return -1;  // empty slot: not present
        if (strncmp(entries_[idx].path, path, PATH_LEN - 1) == 0) return idx;
    }
    return -1;  // unreachable while count_ <= CAP < SLOTS
}

void PathStore::clear() {
    lock_();
    for (int i = 0; i < SLOTS; ++i)
        slots_[i] = -1;
    count_ = 0;
    unlock_();
}

bool PathStore::set(const char *path, double value) {
    if (!path || !path[0]) return false;
    lock_();
    uint32_t h = fnv1a_bounded(path);
    int free_slot = -1;
    int idx = -1;
    for (int step = 0; step < SLOTS; ++step) {
        int slot = (int)((h + (uint32_t)step) & (SLOTS - 1));
        int16_t e = slots_[slot];
        if (e < 0) {
            free_slot = slot;
            break;
        }
        if (strncmp(entries_[e].path, path, PATH_LEN - 1) == 0) {
            idx = e;
            break;
        }
    }
    if (idx < 0) {
        if (count_ >= CAP || free_slot < 0) {
            unlock_();
            return false;  // full: existing paths still update, new ones don't
        }
        idx = count_++;
        strncpy(entries_[idx].path, path, PATH_LEN - 1);
        entries_[idx].path[PATH_LEN - 1] = 0;
        slots_[free_slot] = (int16_t)idx;
    }
    entries_[idx].value = value;
    unlock_();
    return true;
}

double PathStore::get(const char *path) const {
#ifdef DBG_PERF_COUNTERS
    ++lookups_;
#endif
    lock_();
    int i = find_(path);
    double v = i < 0 ? NAN : entries_[i].value;
    unlock_();
    return v;
}

bool PathStore::has(const char *path) const {
#ifdef DBG_PERF_COUNTERS
    ++lookups_;
#endif
    lock_();
    bool present = find_(path) >= 0;
    unlock_();
    return present;
}

const char *PathStore::path_at(int i) const {
    return (i >= 0 && i < count_) ? entries_[i].path : nullptr;
}

double PathStore::value_at(int i) const {
    lock_();
    double v = (i >= 0 && i < count_) ? entries_[i].value : NAN;
    unlock_();
    return v;
}

}  // namespace sk
