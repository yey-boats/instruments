#include "path_store.h"

#include <string.h>

namespace sk {

int PathStore::find_(const char *path) const {
    if (!path) return -1;
    for (int i = 0; i < CAP; ++i) {
        if (entries_[i].used && strncmp(entries_[i].path, path, PATH_LEN) == 0) return i;
    }
    return -1;
}

void PathStore::clear() {
    for (int i = 0; i < CAP; ++i)
        entries_[i].used = false;
    count_ = 0;
}

bool PathStore::set(const char *path, double value) {
    if (!path || !path[0]) return false;
    int i = find_(path);
    if (i < 0) {
        for (int j = 0; j < CAP; ++j) {
            if (!entries_[j].used) {
                i = j;
                break;
            }
        }
        if (i < 0) return false;  // full
        entries_[i].used = true;
        strncpy(entries_[i].path, path, PATH_LEN - 1);
        entries_[i].path[PATH_LEN - 1] = 0;
        ++count_;
    }
    entries_[i].value = value;
    return true;
}

double PathStore::get(const char *path) const {
#ifdef DBG_PERF_COUNTERS
    ++lookups_;
#endif
    int i = find_(path);
    return i < 0 ? NAN : entries_[i].value;
}

bool PathStore::has(const char *path) const {
#ifdef DBG_PERF_COUNTERS
    ++lookups_;
#endif
    return find_(path) >= 0;
}

}  // namespace sk
