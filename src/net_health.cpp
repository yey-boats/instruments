#include "net_health.h"

namespace net_health {

Action evaluate(const Inputs &in, const Thresholds &th) {
    // WS down or never had data: not our recovery path. Caller clears flags.
    if (!in.connected || in.last_update_ms == 0) {
        return Action::Idle;
    }

    // A fresh value-bearing delta landed since the last check.
    if (in.last_update_ms != in.last_seen_update_ms) {
        return Action::Recovered;
    }

    // WS link liveness: if raw frames (PING/PONG/TEXT/BIN) arrived within the
    // first escalation window, the connection is alive and the server is
    // simply not sending value changes (anchored boat, suppressed unchanged
    // values). Don't escalate.
    uint32_t ws_age = (in.ws_frame_ms > 0) ? (in.now_ms - in.ws_frame_ms) : UINT32_MAX;
    if (ws_age < th.reconnect_ms) {
        return Action::Healthy;
    }

    // Both value-bearing deltas and raw frames have been silent. Escalate by
    // data age (measured against the older signal, last_update_ms).
    uint32_t age = in.now_ms - in.last_update_ms;
    if (age >= th.restart_ms) {
        return Action::Restart;
    }
    if (!in.reset_tried && age >= th.reset_ms) {
        return Action::Reset;
    }
    if (!in.reconnect_tried && age >= th.reconnect_ms) {
        return Action::Reconnect;
    }
    return Action::Hold;
}

}  // namespace net_health
