// Host tests for the data-stall escalation decision extracted from
// signalk.cpp::check_stall_autorecover. Pins the ladder behaviour
// (Idle/Recovered/Healthy/Hold/Reconnect/Reset/Restart) so the device-side
// refactor and any future tuning can't silently change when the firmware
// reconnects, resets WiFi, or reboots itself.

#include <unity.h>

#include "net_health.h"

using net_health::Action;
using net_health::evaluate;
using net_health::Inputs;
using net_health::Thresholds;

// Mirrors the live constants: 30 s soft reconnect, 90 s hard reset, 180 s
// device restart.
static Thresholds TH() {
    return Thresholds{30000, 90000, 180000};
}

void setUp(void) {
}
void tearDown(void) {
}

static void test_ws_down_is_idle() {
    Inputs in{};
    in.connected = false;
    in.last_update_ms = 1000;
    in.now_ms = 500000;
    TEST_ASSERT_EQUAL(Action::Idle, evaluate(in, TH()));
}

static void test_never_had_data_is_idle() {
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 0;  // never
    in.now_ms = 500000;
    TEST_ASSERT_EQUAL(Action::Idle, evaluate(in, TH()));
}

static void test_fresh_delta_is_recovered() {
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 400000;       // advanced...
    in.last_seen_update_ms = 350000;  // ...past the prior anchor
    in.now_ms = 401000;
    TEST_ASSERT_EQUAL(Action::Recovered, evaluate(in, TH()));
}

static void test_recent_ws_frame_is_healthy() {
    // Value-bearing delta is stale (anchor unchanged) but raw WS frames are
    // recent: anchored boat sending PING/PONG only. Must NOT escalate.
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;  // no new delta
    in.ws_frame_ms = 495000;          // frame 5 s ago
    in.now_ms = 500000;
    TEST_ASSERT_EQUAL(Action::Healthy, evaluate(in, TH()));
}

static void test_between_thresholds_after_reconnect_is_hold() {
    // 50 s stale (past reconnect, below reset), WS frames also silent, and the
    // soft reconnect was already attempted: nothing more to do this tick.
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;
    in.ws_frame_ms = 100000;     // frames silent too
    in.now_ms = 100000 + 50000;  // age = 50 s
    in.reconnect_tried = true;
    TEST_ASSERT_EQUAL(Action::Hold, evaluate(in, TH()));
}

static void test_30s_triggers_reconnect_once() {
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;
    in.ws_frame_ms = 100000;     // frames also silent
    in.now_ms = 100000 + 35000;  // age = 35 s
    TEST_ASSERT_EQUAL(Action::Reconnect, evaluate(in, TH()));

    // Once reconnect was tried, the same age must not re-fire reconnect.
    in.reconnect_tried = true;
    TEST_ASSERT_EQUAL(Action::Hold, evaluate(in, TH()));
}

static void test_90s_triggers_reset_once() {
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;
    in.ws_frame_ms = 100000;
    in.now_ms = 100000 + 95000;  // age = 95 s
    in.reconnect_tried = true;   // already past reconnect
    TEST_ASSERT_EQUAL(Action::Reset, evaluate(in, TH()));

    in.reset_tried = true;
    TEST_ASSERT_EQUAL(Action::Hold, evaluate(in, TH()));
}

static void test_180s_triggers_restart_regardless_of_flags() {
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;
    in.ws_frame_ms = 100000;
    in.now_ms = 100000 + 200000;  // age = 200 s
    in.reconnect_tried = true;
    in.reset_tried = true;
    TEST_ASSERT_EQUAL(Action::Restart, evaluate(in, TH()));
}

static void test_recent_frame_beats_old_delta_even_when_very_stale() {
    // Delta 200 s stale (would restart) BUT a WS frame arrived 1 s ago:
    // link is alive, so Healthy wins over Restart.
    Inputs in{};
    in.connected = true;
    in.last_update_ms = 100000;
    in.last_seen_update_ms = 100000;
    in.ws_frame_ms = 299000;
    in.now_ms = 300000;  // delta age 200 s, frame age 1 s
    TEST_ASSERT_EQUAL(Action::Healthy, evaluate(in, TH()));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_ws_down_is_idle);
    RUN_TEST(test_never_had_data_is_idle);
    RUN_TEST(test_fresh_delta_is_recovered);
    RUN_TEST(test_recent_ws_frame_is_healthy);
    RUN_TEST(test_between_thresholds_after_reconnect_is_hold);
    RUN_TEST(test_30s_triggers_reconnect_once);
    RUN_TEST(test_90s_triggers_reset_once);
    RUN_TEST(test_180s_triggers_restart_regardless_of_flags);
    RUN_TEST(test_recent_frame_beats_old_delta_even_when_very_stale);
    return UNITY_END();
}
