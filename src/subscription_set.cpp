#include "subscription_set.h"

#include <string.h>

namespace sk {

void SubscriptionSet::clear() {
    // POD zero in place - never `*this = SubscriptionSet{}` (a ~5 KB temporary,
    // the on-stack-large-struct reboot trap from CLAUDE.md).
    memset(paths_, 0, sizeof(paths_));
    count_ = 0;
}

int SubscriptionSet::find_(const char *path) const {
    if (!path || !*path) return -1;
    for (int i = 0; i < count_; ++i)
        if (strcmp(paths_[i], path) == 0) return i;
    return -1;
}

bool SubscriptionSet::add(const char *path) {
    if (!path || !*path) return false;
    if (find_(path) >= 0) return true;  // dedup: already present, no-op
    if (count_ >= CAP) return false;    // full and this is a NEW path
    strncpy(paths_[count_], path, PATH_LEN - 1);
    paths_[count_][PATH_LEN - 1] = '\0';
    ++count_;
    return true;
}

bool SubscriptionSet::has(const char *path) const {
    return find_(path) >= 0;
}

const char *SubscriptionSet::at(int i) const {
    if (i < 0 || i >= count_) return nullptr;
    return paths_[i];
}

void diff(const SubscriptionSet &desired, const SubscriptionSet &active, SubscriptionSet &toAdd,
          SubscriptionSet &toRemove) {
    toAdd.clear();
    toRemove.clear();
    // toAdd = desired - active
    for (int i = 0; i < desired.size(); ++i) {
        const char *p = desired.at(i);
        if (!active.has(p)) toAdd.add(p);
    }
    // toRemove = active - desired
    for (int i = 0; i < active.size(); ++i) {
        const char *p = active.at(i);
        if (!desired.has(p)) toRemove.add(p);
    }
}

}  // namespace sk
