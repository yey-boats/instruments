// Host tests for the blanked record-apply transition (instruments#106):
// include/apply_blank_machine.h decides when the backlight is cut and when it
// comes back. Pins: refused writes never blank, the window spans writes +
// queued activations + the first flushed frame, nested applies restore once,
// and the safety timeouts always restore.

#include <unity.h>

#include "apply_blank_machine.h"

using apply_blank::Action;
using apply_blank::Machine;
using apply_blank::Reason;
using apply_blank::Timing;

void setUp(void) {
}
void tearDown(void) {
}

static Timing T() {
    Timing t;
    t.linger_ms = 150;
    t.idle_timeout_ms = 3000;
    t.max_dark_ms = 10000;
    return t;
}

static void test_idle_machine_never_acts() {
    Machine m(T());
    TEST_ASSERT_FALSE(m.dark());
    TEST_ASSERT_EQUAL(Action::None, m.poll(100000, 0));
    m.expect_render(10);  // a lit-screen activation does not blank
    m.rendered(20, true, 0);
    TEST_ASSERT_FALSE(m.dark());
    TEST_ASSERT_EQUAL(Action::None, m.poll(100000, 5));
}

static void test_write_without_activation_restores_after_linger() {
    Machine m(T());
    uint32_t ep;
    TEST_ASSERT_EQUAL(Action::Cut, m.begin(1000, ep));
    TEST_ASSERT_TRUE(m.dark());
    TEST_ASSERT_EQUAL(Action::None, m.poll(1100, 0));  // write still in flight
    m.end(1300, ep);
    TEST_ASSERT_EQUAL(Action::None, m.poll(1400, 0));  // linger not over
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(1450, 0));
    TEST_ASSERT_EQUAL(Reason::Done, m.last_reason());
    TEST_ASSERT_EQUAL_UINT32(450, m.last_dark_ms());
    TEST_ASSERT_FALSE(m.dark());
    TEST_ASSERT_EQUAL(Action::None, m.poll(1500, 0));  // restore exactly once
}

static void test_activation_waits_for_render_and_first_frame() {
    Machine m(T());
    uint32_t ep;
    m.begin(0, ep);
    m.expect_render(200);  // queued while the guard is still held
    m.end(210, ep);
    TEST_ASSERT_EQUAL(Action::None, m.poll(1000, 7));  // not rendered yet
    m.rendered(1100, true, 7);
    TEST_ASSERT_EQUAL(Action::None, m.poll(1400, 7));  // no new frame flushed
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(1410, 8));
    TEST_ASSERT_EQUAL(Reason::Done, m.last_reason());
}

static void test_failed_activation_does_not_wait_for_a_frame() {
    Machine m(T());
    uint32_t ep;
    m.begin(0, ep);
    m.expect_render(10);
    m.end(20, ep);
    m.rendered(100, false, 3);  // nothing rendered
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(260, 3));
}

static void test_promote_lkg_inside_window_keeps_it_dark() {
    // put -> activation -> (UI task) promote_lkg write -> rendered
    Machine m(T());
    uint32_t ep, ep2;
    m.begin(0, ep);
    m.expect_render(5);
    m.end(6, ep);
    TEST_ASSERT_EQUAL(Action::None, m.begin(300, ep2));  // already dark: no second cut
    TEST_ASSERT_EQUAL_UINT32(ep, ep2);
    m.end(350, ep2);
    m.rendered(360, true, 0);
    TEST_ASSERT_EQUAL(Action::None, m.poll(600, 0));
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(610, 1));
}

static void test_nested_push_restores_once_after_the_last() {
    // DM push: record a, record b, index -- each a separate request.
    Machine m(T());
    uint32_t outer, a, b, ix;
    TEST_ASSERT_EQUAL(Action::Cut, m.begin(0, outer));
    TEST_ASSERT_EQUAL(Action::None, m.begin(10, a));
    m.end(200, a);
    m.expect_render(201);
    TEST_ASSERT_EQUAL(Action::None, m.begin(250, b));
    m.end(400, b);
    TEST_ASSERT_EQUAL(Action::None, m.begin(410, ix));
    m.end(500, ix);
    m.rendered(520, true, 0);
    TEST_ASSERT_EQUAL(Action::None, m.poll(900, 1));  // outer guard still held
    m.end(950, outer);
    TEST_ASSERT_EQUAL(Action::None, m.poll(1000, 1));
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(1100, 1));
    TEST_ASSERT_EQUAL(Action::None, m.poll(1200, 1));
}

static void test_back_to_back_requests_bridged_by_linger() {
    Machine m(T());
    uint32_t a, b;
    m.begin(0, a);
    m.end(100, a);
    TEST_ASSERT_EQUAL(Action::None, m.poll(200, 0));   // 100 ms gap < linger
    TEST_ASSERT_EQUAL(Action::None, m.begin(200, b));  // next PUT: still dark
    m.end(300, b);
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(450, 0));
}

static void test_lost_activation_hits_idle_timeout() {
    Machine m(T());
    uint32_t ep;
    m.begin(0, ep);
    m.expect_render(10);
    m.end(20, ep);
    TEST_ASSERT_EQUAL(Action::None, m.poll(3019, 0));
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(3020, 0));
    TEST_ASSERT_EQUAL(Reason::IdleTimeout, m.last_reason());
}

static void test_stuck_writer_hits_idle_timeout() {
    Machine m(T());
    uint32_t ep;
    m.begin(0, ep);  // never ended
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(3000, 0));
    TEST_ASSERT_EQUAL(Reason::IdleTimeout, m.last_reason());
}

static void test_max_dark_caps_a_busy_window() {
    Machine m(T());
    uint32_t outer, ep;
    m.begin(0, outer);
    for (uint32_t t = 1000; t < 10000; t += 1000) {
        m.begin(t, ep);
        m.end(t + 10, ep);
        TEST_ASSERT_EQUAL(Action::None, m.poll(t + 20, 0));
    }
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(10000, 0));
    TEST_ASSERT_EQUAL(Reason::MaxDark, m.last_reason());
}

static void test_stale_guard_cannot_end_a_new_window() {
    Machine m(T());
    uint32_t old_ep, ep;
    m.begin(0, old_ep);
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(3000, 0));  // timeout reset
    TEST_ASSERT_EQUAL(Action::Cut, m.begin(4000, ep));
    TEST_ASSERT_NOT_EQUAL(old_ep, ep);
    m.end(4010, old_ep);  // the old guard finally unwinds
    TEST_ASSERT_EQUAL(1, m.writers());
    TEST_ASSERT_EQUAL(Action::None, m.poll(4500, 0));
    m.end(4600, ep);
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(4750, 0));
}

static void test_millis_wraparound() {
    Machine m(T());
    uint32_t ep;
    uint32_t t0 = 0xFFFFFF00u;
    m.begin(t0, ep);
    m.end(t0 + 0x50, ep);
    TEST_ASSERT_EQUAL(Action::None, m.poll(t0 + 0x60, 0));
    TEST_ASSERT_EQUAL(Action::Restore, m.poll(t0 + 0x50 + 150, 0));  // wraps past 0
    TEST_ASSERT_EQUAL(Reason::Done, m.last_reason());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_machine_never_acts);
    RUN_TEST(test_write_without_activation_restores_after_linger);
    RUN_TEST(test_activation_waits_for_render_and_first_frame);
    RUN_TEST(test_failed_activation_does_not_wait_for_a_frame);
    RUN_TEST(test_promote_lkg_inside_window_keeps_it_dark);
    RUN_TEST(test_nested_push_restores_once_after_the_last);
    RUN_TEST(test_back_to_back_requests_bridged_by_linger);
    RUN_TEST(test_lost_activation_hits_idle_timeout);
    RUN_TEST(test_stuck_writer_hits_idle_timeout);
    RUN_TEST(test_max_dark_caps_a_busy_window);
    RUN_TEST(test_stale_guard_cannot_end_a_new_window);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
