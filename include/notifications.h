#pragma once

// SignalK-style notifications / alarms store (spec: MFD overhaul, phase 5
// data layer). Producers are the SignalK delta parser (notifications.* paths)
// and the NMEA2000 adapter (Raymarine Seatalk alarm PGN 65288); the consumer
// is the (later-phase) alarm banner plus the `sk-status` debug line.
//
// Pure C++ and header-inline so host Unity suites AND the parser TU can use
// it without a platformio build-filter change (same reasoning as
// n2k_decode.h). The device singleton lives in src/notifications.cpp and is
// PSRAM-allocated per the CLAUDE.md memory-trap rules (~4.3 KB - never a
// .bss static, never a stack local). On the device the store does its own
// locking (SK task + N2K task write, UI task reads), mutex behind
// #ifdef ARDUINO like PathStore.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace notif {

// Severity order is load-bearing: highest_active_severity() returns the max.
// Maps 1:1 onto the SignalK notification states ("normal"/"nominal" both
// collapse to Normal, which CLEARS an entry).
enum class State : uint8_t {
    Normal = 0,
    Alert,
    Warn,
    Alarm,
    Emergency,
};

// Method flags (SignalK notification "method" array).
constexpr uint8_t METHOD_VISUAL = 0x01;
constexpr uint8_t METHOD_SOUND = 0x02;

inline const char *state_name(State s) {
    switch (s) {
    case State::Normal:
        return "normal";
    case State::Alert:
        return "alert";
    case State::Warn:
        return "warn";
    case State::Alarm:
        return "alarm";
    case State::Emergency:
        return "emergency";
    default:
        return "?";
    }
}

// SignalK state string -> State. Unknown strings conservatively map to Alert
// (a producer raised SOMETHING - do not silently drop it); "normal" and
// "nominal" map to Normal (clears). NULL clears too (a nulled-out value on a
// notifications.* path means the alarm went away).
inline State state_from_string(const char *s) {
    if (!s) return State::Normal;
    if (strcmp(s, "normal") == 0 || strcmp(s, "nominal") == 0) return State::Normal;
    if (strcmp(s, "alert") == 0) return State::Alert;
    if (strcmp(s, "warn") == 0 || strcmp(s, "warning") == 0) return State::Warn;
    if (strcmp(s, "alarm") == 0) return State::Alarm;
    if (strcmp(s, "emergency") == 0) return State::Emergency;
    return State::Alert;
}

struct Entry {
    // Path suffix after "notifications." (e.g. "navigation.anchor",
    // "mob", "seatalk.alarm.1.15"). The key for upserts.
    char path[64] = {0};
    char message[96] = {0};
    State state = State::Normal;
    uint8_t method = 0;  // METHOD_* flags
    bool acknowledged = false;
    uint32_t first_ms = 0;  // when this alarm first appeared
    uint32_t last_ms = 0;   // last time a producer re-asserted it
};

class Store {
  public:
    static constexpr int CAP = 24;
    // Entries not re-asserted for this long get dropped by expire(): a
    // producer that vanished (SK server gone, N2K device off-bus) must not
    // pin a stale alarm on screen forever. SignalK producers re-emit
    // periodically; 10 min is generous.
    static constexpr uint32_t DEFAULT_EXPIRE_MS = 10u * 60u * 1000u;

    Store() {
#ifdef ARDUINO
        mtx_ = xSemaphoreCreateMutex();
#endif
    }

    void clear() {
        lock_();
        count_ = 0;
        unlock_();
    }

    // Upsert by path. state == Normal REMOVES the entry (alarm cleared).
    // A re-assert refreshes last_ms; a state CHANGE also clears the ack (an
    // alarm that escalated must re-alert the crew). When full, the incoming
    // entry evicts the lowest-severity (acked-first, then oldest) existing
    // entry only if it is at least as severe. Returns the entry index, or
    // -1 (cleared / dropped).
    int upsert(const char *path, State state, const char *message, uint8_t method,
               uint32_t now_ms) {
        if (!path || !path[0]) return -1;
        lock_();
        int i = find_(path);
        if (state == State::Normal) {
            if (i >= 0) remove_(i);
            unlock_();
            return -1;
        }
        if (i < 0) {
            if (count_ >= CAP) {
                int victim = evict_candidate_();
                if (victim < 0 || entries_[victim].state > state) {
                    unlock_();
                    return -1;  // everything stored outranks the newcomer
                }
                remove_(victim);
            }
            i = count_++;
            Entry &e = entries_[i];
            e = Entry{};
            copy_(e.path, sizeof(e.path), path);
            e.first_ms = now_ms;
        }
        Entry &e = entries_[i];
        if (e.state != state) e.acknowledged = false;  // escalation re-alerts
        e.state = state;
        e.method = method;
        e.last_ms = now_ms;
        copy_(e.message, sizeof(e.message), message ? message : "");
        unlock_();
        return i;
    }

    // Highest severity among UNACKNOWLEDGED entries (Normal when none).
    // The banner sounds/paints from this.
    State highest_active_severity() const {
        lock_();
        State hi = State::Normal;
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].acknowledged) continue;
            if (entries_[i].state > hi) hi = entries_[i].state;
        }
        unlock_();
        return hi;
    }

    // Highest severity including acknowledged entries (for the list UI).
    State highest_severity() const {
        lock_();
        State hi = State::Normal;
        for (int i = 0; i < count_; ++i)
            if (entries_[i].state > hi) hi = entries_[i].state;
        unlock_();
        return hi;
    }

    int count() const {
        lock_();
        int n = count_;
        unlock_();
        return n;
    }

    // Copy entry i (UI-facing: copy under the lock, render from the copy).
    bool get(int i, Entry &out) const {
        lock_();
        if (i < 0 || i >= count_) {
            unlock_();
            return false;
        }
        out = entries_[i];
        unlock_();
        return true;
    }

    // Mark entry i acknowledged (silences it for highest_active_severity()).
    bool acknowledge(int i) {
        lock_();
        if (i < 0 || i >= count_) {
            unlock_();
            return false;
        }
        entries_[i].acknowledged = true;
        unlock_();
        return true;
    }

    // Acknowledge by path (banner buttons address alarms by key, not index).
    bool acknowledge(const char *path) {
        lock_();
        int i = find_(path);
        if (i >= 0) entries_[i].acknowledged = true;
        unlock_();
        return i >= 0;
    }

    // Drop entries not re-asserted within max_age_ms. Returns #removed.
    int expire(uint32_t now_ms, uint32_t max_age_ms = DEFAULT_EXPIRE_MS) {
        lock_();
        int removed = 0;
        for (int i = count_ - 1; i >= 0; --i) {
            if ((uint32_t)(now_ms - entries_[i].last_ms) > max_age_ms) {
                remove_(i);
                ++removed;
            }
        }
        unlock_();
        return removed;
    }

  private:
    Entry entries_[CAP];
    int count_ = 0;
#ifdef ARDUINO
    SemaphoreHandle_t mtx_ = nullptr;
    void lock_() const {
        if (mtx_) xSemaphoreTake(mtx_, portMAX_DELAY);
    }
    void unlock_() const {
        if (mtx_) xSemaphoreGive(mtx_);
    }
#else
    void lock_() const {}
    void unlock_() const {}
#endif

    static void copy_(char *dst, size_t cap, const char *src) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = 0;
    }
    int find_(const char *path) const {
        if (!path) return -1;
        // Compare on the STORED width: an over-long path that was truncated
        // on insert must dedup against itself on the next upsert.
        for (int i = 0; i < count_; ++i)
            if (strncmp(entries_[i].path, path, sizeof(entries_[i].path) - 1) == 0) return i;
        return -1;
    }
    void remove_(int i) {
        for (int j = i; j < count_ - 1; ++j)
            entries_[j] = entries_[j + 1];
        --count_;
    }
    // Least-worth-keeping entry: acknowledged beats severity beats age.
    int evict_candidate_() const {
        int best = -1;
        for (int i = 0; i < count_; ++i) {
            if (best < 0) {
                best = i;
                continue;
            }
            const Entry &a = entries_[i], &b = entries_[best];
            if (a.acknowledged != b.acknowledged) {
                if (a.acknowledged) best = i;
            } else if (a.state != b.state) {
                if (a.state < b.state) best = i;
            } else if ((int32_t)(a.last_ms - b.last_ms) < 0) {
                best = i;  // older re-assert loses
            }
        }
        return best;
    }
};

// Global device singleton (PSRAM-allocated; see src/notifications.cpp).
// Host tests instantiate their own local Store instead.
Store &store();

// One-line status summary for `sk-status`-style debugging, e.g.
// "notif=2 highest=alarm unacked=1". Returns chars written (snprintf-style).
int status_line(char *buf, size_t cap);

}  // namespace notif
