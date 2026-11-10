#pragma once
// Hand-written pure protocol logic: version/compat, auth, the target-side
// session table. No Arduino/LVGL/BLE — host-testable (links in the native env).
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "proto/records_generated.h"

namespace proto {

constexpr int kMaxSessions = 8;
constexpr long kDefaultTtlMs = 10000;

// "1.7" -> major 1, minor 7. Returns false on malformed input.
bool parse_version(const char *v, int &major, int &minor);
// Same-major required; our minor must be >= 0; unknown-minor accepted.
bool version_compatible(const char *peer_v);

// Our own protocol version, "<kProtoMajor>.<kProtoMinor>" (e.g. "2.0"). Every
// record, reply and controller request the firmware emits is stamped with this
// -- never a string literal (instruments#25: v0.4.x stamped "1.0" while
// requiring major 2 inbound).
const char *version_string();
// Stamp a generated record's `v` field (char v[N]) with version_string().
void stamp_version(char *v, size_t cap);
template <size_t N> inline void stamp_version(char (&v)[N]) {
    stamp_version(v, N);
}

// Shared-key check. configured_key == "" => open (accept any). Constant-ish compare.
bool auth_ok(const char *configured_key, const char *presented_key);

// Target-side session table (pure; the firmware feeds it now-millis).
struct SessionTable {
    Session sessions[kMaxSessions];
    char sessionId[kMaxSessions][16];
    bool used[kMaxSessions] = {false};
    long mostRecentIdx = -1;

    void clear();
    // Attach: returns index >=0 (assigns sessionId_out), or -1 if full.
    int attach(const Attach &a, long now_ms, char *sessionId_out, size_t cap);
    // Returns true and refreshes lastSeen; false if unknown sessionId.
    bool heartbeat(const char *sid, long now_ms);
    bool detach(const char *sid);
    // Reap sessions with lastSeen older than ttl_ms; returns count reaped.
    int reap(long now_ms, long ttl_ms);
    int active_count() const;
    // Fill a ControlState for serialization.
    void to_control_state(ControlState &out, const char *currentView) const;
};

}  // namespace proto
