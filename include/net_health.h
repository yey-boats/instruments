#pragma once

// Pure-C++ data-stall escalation decision, extracted from
// signalk.cpp::check_stall_autorecover so it can be unit-tested on the host.
//
// The device's network can enter a "half-up" state where WiFi.status() still
// claims STA-connected but no traffic flows. Rather than trust the driver
// status, this verdict keys off actual data liveness: value-bearing SignalK
// deltas (last_update_ms) AND raw WebSocket frames (ws_frame_ms, which still
// advance via PING/PONG on an anchored boat that sends no value changes).
//
// evaluate() is a pure function: it reads the liveness inputs + the prior
// escalation flags and returns the single action the caller should take. The
// caller (signalk.cpp) owns all side effects (dispatchCommand, WiFi teardown,
// ESP.restart) and persists the flags/anchor it returns to.

#include <stdbool.h>
#include <stdint.h>

namespace net_health {

// Escalation ladder, in increasing severity. The caller maps each to a side
// effect; Hold/Healthy/Recovered/Idle are no-ops beyond flag bookkeeping.
enum class Action {
    Idle,       // WS down or never had data: clear flags, wait. Not our path.
    Recovered,  // a fresh delta landed: clear escalation, re-anchor.
    Healthy,    // WS frames recent: link alive, server just quiet. Clear flags.
    Hold,       // stale but below the first threshold: do nothing yet.
    Reconnect,  // soft wifi-reconnect (first escalation).
    Reset,      // hard WiFi disconnect+erase+reassoc (second escalation).
    Restart,    // ESP.restart() (last resort; caller applies boot-loop guard).
};

struct Thresholds {
    uint32_t reconnect_ms;  // soft reconnect at/after this data age
    uint32_t reset_ms;      // hard reset at/after this data age
    uint32_t restart_ms;    // device restart at/after this data age
};

struct Inputs {
    bool connected;                // WS connected
    uint32_t last_update_ms;       // last value-bearing delta (0 = never)
    uint32_t ws_frame_ms;          // last raw WS frame (0 = never)
    uint32_t now_ms;               // current millis()
    uint32_t last_seen_update_ms;  // anchor remembered from the prior call
    bool reconnect_tried;          // soft reconnect already attempted this stall
    bool reset_tried;              // hard reset already attempted this stall
};

// Decide the single action for this tick. Pure; no side effects.
Action evaluate(const Inputs &in, const Thresholds &th);

}  // namespace net_health
