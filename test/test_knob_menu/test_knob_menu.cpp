#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "knob_menu.h"

using namespace knob;

void setUp(void) {
}
void tearDown(void) {
}

static Inputs base_inputs() {
    Inputs in;
    in.ap_state = "standby";
    in.ap_target_rad = NAN;
    in.heading_rad = M_PI;  // 180 deg
    in.display_count = 0;
    in.view_count = 0;
    return in;
}

static void test_home_cw_small_step_presets_target_from_heading() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::DetentCW, /*held=*/false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    // seeded from heading 180, +1 deg -> 181 deg
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 181.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_cw_big_step_when_held() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::DetentCW, /*held=*/true);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 185.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_ccw_wraps_below_zero() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    in.heading_rad = 0.0;
    Action a = step(m, in, Event::DetentCCW, /*held=*/false);
    // 0 - 1 deg -> 359 deg
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 359.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_adjust_accumulates_into_pending() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    step(m, in, Event::DetentCW, false);             // 181
    Action a = step(m, in, Event::DetentCW, false);  // 182
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 182.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_click_engages_last_mode_then_standby() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    // default last_engaged_mode = 1 (COMPASS -> "auto")
    Action a1 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetState, (int)a1.type);
    TEST_ASSERT_EQUAL_STRING("auto", a1.arg_str);
    TEST_ASSERT_TRUE(m.engaged);
    // second click disengages -> standby
    Action a2 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_STRING("standby", a2.arg_str);
    TEST_ASSERT_FALSE(m.engaged);
}

static void test_home_longpress_opens_mode_picker() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::LongPress, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::ModePicker, (int)m.level);
    TEST_ASSERT_EQUAL_INT(1, m.highlight);  // default last_engaged_mode = 1 (COMPASS)
}

static void test_mode_picker_scroll_and_select_wind() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);  // -> ModePicker, highlight=last(1)
    step(m, in, Event::DetentCW, false);   // highlight 2 (WIND)
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetState, (int)a.type);
    TEST_ASSERT_EQUAL_STRING("wind", a.arg_str);
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);  // returns home
    TEST_ASSERT_TRUE(m.engaged);
    TEST_ASSERT_EQUAL_INT(2, m.last_engaged_mode);
}

static void test_mode_picker_doubleclick_cancels() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);
    Action a = step(m, in, Event::DoubleClick, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
}

static void test_mode_picker_select_standby_disengages() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);  // highlight = 1
    step(m, in, Event::DetentCCW, false);  // highlight 0 (STANDBY)
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_STRING("standby", a.arg_str);
    TEST_ASSERT_FALSE(m.engaged);
}

static void test_home_adjust_seeds_from_ap_target_when_no_pending() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    in.ap_target_rad = 90.0 * M_PI / 180.0;  // known target 90 deg
    in.heading_rad = 180.0 * M_PI / 180.0;   // different heading
    Action a = step(m, in, Event::DetentCW, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 91.0 * M_PI / 180.0, a.arg_f);  // 90 + 1, not 181
}

static Inputs remote_inputs() {
    Inputs in = base_inputs();
    in.display_count = 3;
    in.view_count = 4;
    return in;
}

static void test_select_display_scroll_and_drill_in() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);  // Home -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::DetentCW, false);          // highlight 1
    step(m, in, Event::DetentCW, false);          // highlight 2
    Action a = step(m, in, Event::Click, false);  // drill into display 2
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
    TEST_ASSERT_EQUAL_INT(2, m.entered_display);
    TEST_ASSERT_EQUAL_INT(0, m.highlight);
}

static void test_select_display_wraps() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);
    step(m, in, Event::DetentCCW, false);  // 0 -> wrap to 2
    TEST_ASSERT_EQUAL_INT(2, m.highlight);
}

static void test_select_view_click_emits_switch_view() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);  // SelectDisplay
    step(m, in, Event::Click, false);        // enter display 0
    step(m, in, Event::DetentCW, false);     // view highlight 1
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::SwitchView, (int)a.type);
    TEST_ASSERT_EQUAL_INT(0, a.arg_dev_idx);
    TEST_ASSERT_EQUAL_INT(1, a.arg_view_idx);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);  // stays
}

static void test_select_view_ccw_wraps() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);  // Home -> SelectDisplay
    step(m, in, Event::Click, false);        // -> SelectView, highlight 0
    step(m, in, Event::DetentCCW, false);    // 0 -> wrap to 3
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
    TEST_ASSERT_EQUAL_INT(3, m.highlight);
}

static void test_select_view_stays_on_repeated_clicks() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);  // SelectDisplay
    step(m, in, Event::Click, false);        // SelectView
    Action a1 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::SwitchView, (int)a1.type);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
    Action a2 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::SwitchView, (int)a2.type);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
}

static void test_double_click_back_chain() {
    Model m;
    init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);  // Home -> SelectDisplay
    step(m, in, Event::Click, false);        // -> SelectView
    step(m, in, Event::DoubleClick, false);  // -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::DoubleClick, false);  // -> Home
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
}

// =====================================================================
//  signalk_put_for: pure SignalK-PUT contract (shared with knob_ui.cpp)
// =====================================================================

static void test_signalk_put_for_state() {
    Action a;
    a.type = ActionType::ApSetState;
    strncpy(a.arg_str, "auto", sizeof(a.arg_str) - 1);
    char path[64] = {0xF};
    char value[64] = {0xF};
    bool put = signalk_put_for(a, path, sizeof(path), value, sizeof(value));
    TEST_ASSERT_TRUE(put);
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.state", path);
    TEST_ASSERT_EQUAL_STRING("\"auto\"", value);
}

static void test_signalk_put_for_target_format() {
    Action a;
    a.type = ActionType::ApSetTargetRad;
    a.arg_f = 1.5708;  // ~90 deg
    char path[64] = {0};
    char value[64] = {0};
    bool put = signalk_put_for(a, path, sizeof(path), value, sizeof(value));
    TEST_ASSERT_TRUE(put);
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.target.headingTrue", path);
    TEST_ASSERT_EQUAL_STRING("1.5708", value);  // %.4f of 1.5708
}

static void test_signalk_put_for_noaction_and_switchview_return_false() {
    char path[64] = {0xF};
    char value[64] = {0xF};

    Action none;  // NoAction
    bool p1 = signalk_put_for(none, path, sizeof(path), value, sizeof(value));
    TEST_ASSERT_FALSE(p1);
    TEST_ASSERT_EQUAL_STRING("", path);
    TEST_ASSERT_EQUAL_STRING("", value);

    Action sw;
    sw.type = ActionType::SwitchView;
    sw.arg_dev_idx = 2;
    sw.arg_view_idx = 1;
    path[0] = 0xF;
    value[0] = 0xF;
    bool p2 = signalk_put_for(sw, path, sizeof(path), value, sizeof(value));
    TEST_ASSERT_FALSE(p2);
    TEST_ASSERT_EQUAL_STRING("", path);
    TEST_ASSERT_EQUAL_STRING("", value);
}

static void test_signalk_put_for_buffer_cap_safety() {
    Action a;
    a.type = ActionType::ApSetTargetRad;
    a.arg_f = 3.14159;
    // Tiny caps: must null-terminate within bounds and not overflow.
    char path[4];
    char value[4];
    memset(path, 0x7F, sizeof(path));
    memset(value, 0x7F, sizeof(value));
    bool put = signalk_put_for(a, path, sizeof(path), value, sizeof(value));
    TEST_ASSERT_TRUE(put);
    // snprintf always null-terminates within cap.
    TEST_ASSERT_EQUAL_CHAR(0, path[sizeof(path) - 1]);
    TEST_ASSERT_EQUAL_CHAR(0, value[sizeof(value) - 1]);
    TEST_ASSERT_TRUE(strlen(path) < sizeof(path));
    TEST_ASSERT_TRUE(strlen(value) < sizeof(value));
    // The truncated path is still a prefix of the full path.
    TEST_ASSERT_EQUAL_STRING("ste", path);
}

// Small helper: assert an Action yields the AP-state PUT we expect.
static void assert_state_put(const Action &a, const char *expect_state) {
    char path[64], value[64];
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetState, (int)a.type);
    TEST_ASSERT_TRUE(signalk_put_for(a, path, sizeof(path), value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.state", path);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "\"%s\"", expect_state);
    TEST_ASSERT_EQUAL_STRING(vbuf, value);
}

// Small helper: assert an Action yields the AP-target PUT for expected radians.
static void assert_target_put(const Action &a, double expect_rad) {
    char path[64], value[64];
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, expect_rad, a.arg_f);
    TEST_ASSERT_TRUE(signalk_put_for(a, path, sizeof(path), value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.target.headingTrue", path);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "%.4f", expect_rad);
    TEST_ASSERT_EQUAL_STRING(vbuf, value);
}

static double deg2rad(double d) {
    return d * M_PI / 180.0;
}

// =====================================================================
//  Gesture journeys: realistic multi-step operation + formatted PUTs
// =====================================================================

// Journey 1: engage compass, nudge to +15 deg from heading 100, disengage.
static void test_journey_engage_nudge_disengage() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    in.heading_rad = deg2rad(100.0);  // known seed

    // Click -> engage last mode (COMPASS = idx 1 -> "auto").
    Action engage = step(m, in, Event::Click, false);
    assert_state_put(engage, "auto");
    TEST_ASSERT_TRUE(m.engaged);

    // Now nudge +15 deg. The state machine seeds the first detent from the
    // heading (100), then accumulates into pending_target. Use a mix of held
    // (+5) and unheld (+1) detents: +5 +5 +1 +1 +1 +1 +1 = +15 -> 115 deg.
    Action a;
    a = step(m, in, Event::DetentCW, /*held=*/true);
    assert_target_put(a, deg2rad(105.0));
    a = step(m, in, Event::DetentCW, /*held=*/true);
    assert_target_put(a, deg2rad(110.0));
    a = step(m, in, Event::DetentCW, /*held=*/false);
    assert_target_put(a, deg2rad(111.0));
    a = step(m, in, Event::DetentCW, /*held=*/false);
    assert_target_put(a, deg2rad(112.0));
    a = step(m, in, Event::DetentCW, /*held=*/false);
    assert_target_put(a, deg2rad(113.0));
    a = step(m, in, Event::DetentCW, /*held=*/false);
    assert_target_put(a, deg2rad(114.0));
    a = step(m, in, Event::DetentCW, /*held=*/false);
    assert_target_put(a, deg2rad(115.0));  // +15 total

    // Click -> disengage -> standby.
    Action disengage = step(m, in, Event::Click, false);
    assert_state_put(disengage, "standby");
    TEST_ASSERT_FALSE(m.engaged);
}

// Journey 2: enter WIND via the mode picker, then a Home detent still emits
// an ApSetTargetRad (device treats it as a wind-angle adjust).
static void test_journey_wind_mode_then_adjust() {
    Model m;
    init(m);
    Inputs in = base_inputs();
    in.heading_rad = deg2rad(200.0);

    step(m, in, Event::LongPress, false);  // -> ModePicker, highlight = 1 (COMPASS)
    step(m, in, Event::DetentCW, false);   // highlight 2 (WIND)
    Action sel = step(m, in, Event::Click, false);
    assert_state_put(sel, "wind");
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
    TEST_ASSERT_TRUE(m.engaged);

    // Back Home, a detent still produces an ApSetTargetRad (same machine output).
    Action a = step(m, in, Event::DetentCW, false);  // seed 200 + 1 -> 201
    assert_target_put(a, deg2rad(201.0));
}

// Journey 3: remote — pick display 2, pick a view, switch. SwitchView is NOT a
// PUT (signalk_put_for returns false).
static void test_journey_remote_switch_view() {
    Model m;
    init(m);
    Inputs in = remote_inputs();  // display_count=3, view_count=4

    step(m, in, Event::DoubleClick, false);           // Home -> SelectDisplay (highlight 0)
    step(m, in, Event::DetentCW, false);              // display highlight 1
    step(m, in, Event::DetentCW, false);              // display highlight 2
    Action drill = step(m, in, Event::Click, false);  // enter display 2 -> SelectView
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)drill.type);
    TEST_ASSERT_EQUAL_INT(2, m.entered_display);

    step(m, in, Event::DetentCW, false);  // view highlight 1
    step(m, in, Event::DetentCW, false);  // view highlight 2
    Action sw = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::SwitchView, (int)sw.type);
    TEST_ASSERT_EQUAL_INT(2, sw.arg_dev_idx);
    TEST_ASSERT_EQUAL_INT(2, sw.arg_view_idx);

    // SwitchView is not a SignalK PUT.
    char path[64] = {0xF};
    char value[64] = {0xF};
    TEST_ASSERT_FALSE(signalk_put_for(sw, path, sizeof(path), value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("", path);
    TEST_ASSERT_EQUAL_STRING("", value);
}

// Journey 4: back-navigation — Home -> SelectDisplay -> SelectView, then
// DoubleClick back up to SelectDisplay, then back to Home. Assert levels.
static void test_journey_back_navigation() {
    Model m;
    init(m);
    Inputs in = remote_inputs();

    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
    step(m, in, Event::DoubleClick, false);  // -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::Click, false);  // -> SelectView
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
    step(m, in, Event::DoubleClick, false);  // back -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::DoubleClick, false);  // back -> Home
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_home_cw_small_step_presets_target_from_heading);
    RUN_TEST(test_home_cw_big_step_when_held);
    RUN_TEST(test_home_ccw_wraps_below_zero);
    RUN_TEST(test_home_adjust_accumulates_into_pending);
    RUN_TEST(test_home_click_engages_last_mode_then_standby);
    RUN_TEST(test_home_longpress_opens_mode_picker);
    RUN_TEST(test_home_adjust_seeds_from_ap_target_when_no_pending);
    RUN_TEST(test_mode_picker_scroll_and_select_wind);
    RUN_TEST(test_mode_picker_doubleclick_cancels);
    RUN_TEST(test_mode_picker_select_standby_disengages);
    RUN_TEST(test_select_display_scroll_and_drill_in);
    RUN_TEST(test_select_display_wraps);
    RUN_TEST(test_select_view_click_emits_switch_view);
    RUN_TEST(test_select_view_ccw_wraps);
    RUN_TEST(test_select_view_stays_on_repeated_clicks);
    RUN_TEST(test_double_click_back_chain);
    // signalk_put_for unit tests
    RUN_TEST(test_signalk_put_for_state);
    RUN_TEST(test_signalk_put_for_target_format);
    RUN_TEST(test_signalk_put_for_noaction_and_switchview_return_false);
    RUN_TEST(test_signalk_put_for_buffer_cap_safety);
    // gesture journeys
    RUN_TEST(test_journey_engage_nudge_disengage);
    RUN_TEST(test_journey_wind_mode_then_adjust);
    RUN_TEST(test_journey_remote_switch_view);
    RUN_TEST(test_journey_back_navigation);
    return UNITY_END();
}
