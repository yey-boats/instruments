#pragma once

// Pure multilevel knob menu state machine. No LVGL, no Arduino — links
// on the native host test env. The device glue (knob_ui / main) feeds it
// Events + a read-only Inputs snapshot and turns the emitted Action into
// the existing app::Command path. Levels and gestures per
// docs/superpowers/specs/2026-06-13-waveshare-knob-remote-design.md §3.4.

#include <stddef.h>
#include <stdint.h>

namespace knob {

constexpr int kStepSmallDeg = 1;
constexpr int kStepBigDeg = 5;
constexpr int kModeCount = 4;  // STANDBY, COMPASS, WIND, ROUTE

enum class Event : uint8_t { None = 0, DetentCW, DetentCCW, Click, DoubleClick, LongPress };

enum class Level : uint8_t { Home = 0, ModePicker, SelectDisplay, SelectView };

enum class ActionType : uint8_t {
    NoAction = 0,
    ApSetState,      // arg_str = "standby"|"auto"|"wind"|"route"
    ApSetTargetRad,  // arg_f = absolute target heading (radians, 0..2pi)
    SwitchView,      // arg_dev_idx + arg_view_idx -> switch that display's view
};

struct Action {
    ActionType type = ActionType::NoAction;
    char arg_str[16] = {0};
    double arg_f = 0.0;
    int arg_dev_idx = -1;
    int arg_view_idx = -1;
};

// Read-only snapshot the menu reasons over.
struct Inputs {
    const char *ap_state = "";   // current AP state string ("standby"/"auto"/...)
    double ap_target_rad = 0.0;  // NaN if unknown
    double heading_rad = 0.0;    // NaN if unknown
    int display_count = 0;       // # of displays in SelectDisplay
    int view_count = 0;          // # of views of the entered display (SelectView)
};

struct Model {
    Level level = Level::Home;
    int highlight = 0;                // index within the current level's list
    int entered_display = -1;         // display drilled into (SelectView)
    double pending_target_rad = 0.0;  // local pending target; NaN until adjusted
    int last_engaged_mode = 1;        // picker idx of last non-standby mode (COMPASS)
    bool engaged = false;             // AP engaged (non-standby)?
};

void init(Model &m);

// Step the machine. `held` = button held during a detent (=> ±5 instead of ±1).
// Pure: mutates `m`, returns the side-effect Action (NoAction if none).
Action step(Model &m, const Inputs &in, Event ev, bool held);

// 0->"standby", 1->"auto", 2->"wind", 3->"route". Out-of-range -> "standby".
const char *mode_state_string(int picker_idx);
const char *level_name(Level l);

// Fills the SignalK PUT path + JSON value for an autopilot Action. This is the
// single source of truth for the SignalK contract — both the device dispatch
// (knob_ui.cpp) and the host tests format PUTs through it.
//   ApSetState     -> path "steering/autopilot/state",
//                     value "\"<arg_str>\"" (JSON-quoted, e.g. "auto")
//   ApSetTargetRad -> path "steering/autopilot/target/headingTrue",
//                     value "%.4f" of arg_f (radians)
//   else (NoAction / SwitchView) -> returns false, leaves outputs empty
//                                    (null-terminated path_out[0]/value_out[0]).
// Returns true when a PUT was produced, false otherwise.
bool signalk_put_for(const Action &a, char *path_out, size_t path_cap, char *value_out,
                     size_t value_cap);

}  // namespace knob
