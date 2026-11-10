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

const char *version_string() {
    // Built once from the generated constants so a schema bump flows through.
    static char buf[16] = {0};
    if (!buf[0]) snprintf(buf, sizeof(buf), "%d.%d", kProtoMajor, kProtoMinor);
    return buf;
}

void stamp_version(char *v, size_t cap) {
    if (!v || cap == 0) return;
    snprintf(v, cap, "%s", version_string());
}

static size_t finish(int n, char *buf, size_t cap) {
    if (n < 0 || (size_t)n >= cap) {
        if (cap) buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t control_error_json(const char *reason, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    int n = snprintf(buf, cap, "{\"v\":\"%s\",\"ok\":false,\"reason\":\"%s\"}", version_string(),
                     reason ? reason : "");
    return finish(n, buf, cap);
}

size_t control_ready_json(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    int n = snprintf(buf, cap, "{\"v\":\"%s\",\"t\":\"ready\"}", version_string());
    return finish(n, buf, cap);
}

// Compare without an early exit on the first differing byte. The length of
// the configured key still leaks through timing; its content does not.
static bool key_equal(const char *configured, const char *presented) {
    size_t n = strlen(configured);
    size_t m = strnlen(presented, n + 1);
    unsigned char diff = (unsigned char)(n != m);
    for (size_t i = 0; i < n; ++i) {
        unsigned char p = i < m ? (unsigned char)presented[i] : 0;
        diff |= (unsigned char)configured[i] ^ p;
    }
    return diff == 0;
}

bool auth_ok(const char *configured_key, const char *presented_key) {
    if (!configured_key || !configured_key[0]) return true;  // open
    if (!presented_key) return false;
    return key_equal(configured_key, presented_key);
}

bool control_auth_required(const char *configured_key, bool paired) {
    return paired || (configured_key && configured_key[0]);
}

bool control_auth_ok(const char *configured_key, const char *presented_key, bool paired) {
    if (!control_auth_required(configured_key, paired)) return true;
    // Paired with no key configured: nothing can match.
    if (!configured_key || !configured_key[0]) return false;
    return auth_ok(configured_key, presented_key);
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
    stamp_version(out.v);
    strncpy(out.currentView, currentView ? currentView : "", sizeof(out.currentView) - 1);
    out.sessions_count = 0;
    for (int i = 0; i < kMaxSessions; ++i) {
        if (!used[i] || out.sessions_count >= 16) continue;
        out.sessions[out.sessions_count++] = sessions[i];
    }
}

}  // namespace proto
