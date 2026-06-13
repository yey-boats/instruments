#include "knob_menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace knob {

static const char *kModeState[kModeCount] = {"standby", "auto", "wind", "route"};

const char *mode_state_string(int idx) {
    if (idx < 0 || idx >= kModeCount) return "standby";
    return kModeState[idx];
}

const char *level_name(Level l) {
    switch (l) {
    case Level::Home:
        return "home";
    case Level::ModePicker:
        return "mode";
    case Level::SelectDisplay:
        return "display";
    case Level::SelectView:
        return "view";
    }
    return "?";
}

bool signalk_put_for(const Action &a, char *path_out, size_t path_cap, char *value_out,
                     size_t value_cap) {
    if (path_out && path_cap) path_out[0] = 0;
    if (value_out && value_cap) value_out[0] = 0;
    switch (a.type) {
    case ActionType::ApSetState:
        if (path_out && path_cap) {
            snprintf(path_out, path_cap, "%s", "steering/autopilot/state");
        }
        if (value_out && value_cap) {
            snprintf(value_out, value_cap, "\"%s\"", a.arg_str);
        }
        return true;
    case ActionType::ApSetTargetRad:
        if (path_out && path_cap) {
            snprintf(path_out, path_cap, "%s", "steering/autopilot/target/headingTrue");
        }
        if (value_out && value_cap) {
            snprintf(value_out, value_cap, "%.4f", a.arg_f);
        }
        return true;
    default:
        return false;
    }
}

void init(Model &m) {
    m = Model{};
    m.pending_target_rad = NAN;
}

static double wrap_2pi(double r) {
    while (r < 0)
        r += 2 * M_PI;
    while (r >= 2 * M_PI)
        r -= 2 * M_PI;
    return r;
}

static double seed_target(const Model &m, const Inputs &in) {
    if (!isnan(m.pending_target_rad)) return m.pending_target_rad;
    if (!isnan(in.ap_target_rad)) return in.ap_target_rad;
    if (!isnan(in.heading_rad)) return in.heading_rad;
    return 0.0;
}

static Action set_state(int picker_idx) {
    Action a;
    a.type = ActionType::ApSetState;
    strncpy(a.arg_str, mode_state_string(picker_idx), sizeof(a.arg_str) - 1);
    return a;
}

static int wrap_idx(int i, int n) {
    if (n <= 0) return 0;
    return ((i % n) + n) % n;
}

static Action adjust(Model &m, const Inputs &in, int sign, bool held) {
    int deg = (held ? kStepBigDeg : kStepSmallDeg) * sign;
    double t = wrap_2pi(seed_target(m, in) + deg * M_PI / 180.0);
    m.pending_target_rad = t;
    Action a;
    a.type = ActionType::ApSetTargetRad;
    a.arg_f = t;
    return a;
}

Action step(Model &m, const Inputs &in, Event ev, bool held) {
    Action a;  // NoAction
    switch (m.level) {
    case Level::Home:
        if (ev == Event::DetentCW) return adjust(m, in, +1, held);
        if (ev == Event::DetentCCW) return adjust(m, in, -1, held);
        if (ev == Event::Click) {
            if (m.engaged) {
                m.engaged = false;
                return set_state(0);  // standby
            }
            m.engaged = true;
            return set_state(m.last_engaged_mode);
        }
        if (ev == Event::LongPress) {
            m.level = Level::ModePicker;
            m.highlight = m.last_engaged_mode;
            return a;
        }
        if (ev == Event::DoubleClick) {
            m.level = Level::SelectDisplay;
            m.highlight = 0;
            return a;
        }
        break;

    case Level::ModePicker:
        if (ev == Event::DetentCW) {
            m.highlight = wrap_idx(m.highlight + 1, kModeCount);
            return a;
        }
        if (ev == Event::DetentCCW) {
            m.highlight = wrap_idx(m.highlight - 1, kModeCount);
            return a;
        }
        if (ev == Event::Click) {
            m.level = Level::Home;
            if (m.highlight == 0) {
                m.engaged = false;
            } else {
                m.engaged = true;
                m.last_engaged_mode = m.highlight;
            }
            return set_state(m.highlight);
        }
        if (ev == Event::DoubleClick) {
            m.level = Level::Home;
            return a;
        }
        break;

    case Level::SelectDisplay:
        if (ev == Event::DetentCW) {
            m.highlight = wrap_idx(m.highlight + 1, in.display_count);
            return a;
        }
        if (ev == Event::DetentCCW) {
            m.highlight = wrap_idx(m.highlight - 1, in.display_count);
            return a;
        }
        if (ev == Event::Click) {
            m.entered_display = m.highlight;
            m.level = Level::SelectView;
            m.highlight = 0;
            return a;
        }
        if (ev == Event::DoubleClick) {
            m.level = Level::Home;
            return a;
        }
        break;

    case Level::SelectView:
        if (ev == Event::DetentCW) {
            m.highlight = wrap_idx(m.highlight + 1, in.view_count);
            return a;
        }
        if (ev == Event::DetentCCW) {
            m.highlight = wrap_idx(m.highlight - 1, in.view_count);
            return a;
        }
        if (ev == Event::Click) {
            a.type = ActionType::SwitchView;
            a.arg_dev_idx = m.entered_display;
            a.arg_view_idx = m.highlight;
            return a;  // stays in SelectView
        }
        if (ev == Event::DoubleClick) {
            m.level = Level::SelectDisplay;
            return a;
        }
        break;

    default:
        break;
    }
    return a;
}

}  // namespace knob
