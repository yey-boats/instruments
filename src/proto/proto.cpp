#include "proto/proto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace proto {

bool parse_version(const char *v, int &major, int &minor) {
    if (!v || !*v) return false;
    char *end = nullptr;
    long M = strtol(v, &end, 10);
    if (end == v || *end != '.') return false;
    long m = strtol(end + 1, &end, 10);
    if (*end != '\0') return false;
    major = (int)M;
    minor = (int)m;
    return true;
}

bool version_compatible(const char *peer_v) {
    int M, m;
    if (!parse_version(peer_v, M, m)) return false;
    return M == kProtoMajor;
}

bool auth_ok(const char *configured_key, const char *presented_key) {
    if (!configured_key || !configured_key[0]) return true;  // open
    if (!presented_key) return false;
    return strcmp(configured_key, presented_key) == 0;
}

void SessionTable::clear() {
    for (int i = 0; i < kMaxSessions; ++i)
        used[i] = false;
    mostRecentIdx = -1;
}

int SessionTable::attach(const Attach &a, long now_ms, char *sid_out, size_t cap) {
    int idx = -1;
    for (int i = 0; i < kMaxSessions; ++i)
        if (!used[i]) {
            idx = i;
            break;
        }
    if (idx < 0) return -1;
    used[idx] = true;
    strncpy(sessions[idx].controllerId, a.controllerId, sizeof(sessions[idx].controllerId) - 1);
    strncpy(sessions[idx].name, a.name, sizeof(sessions[idx].name) - 1);
    strncpy(sessions[idx].color, a.color, sizeof(sessions[idx].color) - 1);
    sessions[idx].lastSeen = now_ms;
    snprintf(sessionId[idx], 16, "s%ld-%d", now_ms % 100000, idx);
    if (sid_out) strncpy(sid_out, sessionId[idx], cap - 1);
    mostRecentIdx = idx;
    return idx;
}

bool SessionTable::heartbeat(const char *sid, long now_ms) {
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && strcmp(sessionId[i], sid) == 0) {
            sessions[i].lastSeen = now_ms;
            mostRecentIdx = i;
            return true;
        }
    return false;
}

bool SessionTable::detach(const char *sid) {
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && strcmp(sessionId[i], sid) == 0) {
            used[i] = false;
            return true;
        }
    return false;
}

int SessionTable::reap(long now_ms, long ttl_ms) {
    int n = 0;
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && now_ms - sessions[i].lastSeen > ttl_ms) {
            used[i] = false;
            n++;
        }
    return n;
}

int SessionTable::active_count() const {
    int n = 0;
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i]) n++;
    return n;
}

void SessionTable::to_control_state(ControlState &out, const char *currentView) const {
    snprintf(out.v, sizeof(out.v), "%d.%d", kProtoMajor, kProtoMinor);
    strncpy(out.currentView, currentView ? currentView : "", sizeof(out.currentView) - 1);
    out.sessions_count = 0;
    for (int i = 0; i < kMaxSessions; ++i) {
        if (!used[i] || out.sessions_count >= 16) continue;
        out.sessions[out.sessions_count++] = sessions[i];
    }
}

}  // namespace proto
