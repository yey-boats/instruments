#pragma once

#include <lvgl.h>

#include "proto/proto.h"  // proto::Session for the control-frame overlay

// Every screen module exposes build() (one-time UI construction, returns the
// root object) and refresh() (called from the global 5 Hz tick when the
// screen is visible).

namespace ui {

namespace dashboard {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace dashboard

// Full-screen single-value view, opened by tapping a dashboard tile.
namespace zoom {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace zoom

namespace wind {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void set_refresh_enabled(bool e);
bool refresh_enabled();
}  // namespace wind

// Original (pre-2026-06 reference redesign) wind dial, kept registered as a
// second screen so the two designs can be compared side by side on device.
namespace wind_classic {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void set_refresh_enabled(bool e);
bool refresh_enabled();
}  // namespace wind_classic

// Wind-steering sailing aid: TWA hero, tack/gybe angles + target headings,
// true wind direction, and apparent/true wind angles + speeds.
namespace wind_steer {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace wind_steer

namespace touch_cal_screen {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace touch_cal_screen

namespace touch_grid_screen {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace touch_grid_screen

namespace demo_grid {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace demo_grid

namespace nav {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace nav

namespace depth {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace depth

namespace status_panel {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace status_panel

namespace steering {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace steering

namespace trip {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void reset();  // zero trip counters via console / BLE
}  // namespace trip

namespace route {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace route

namespace autopilot {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace autopilot

namespace wifi_setup {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace wifi_setup

namespace settings {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace settings

// --- Waveshare knob (round 360x360) dedicated views + menu overlay ---
namespace ap_hud {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace ap_hud
namespace knob_compass {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace knob_compass
namespace knob_wind {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace knob_wind
namespace knob_big {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace knob_big
namespace knob_menu_overlay {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void show(bool on);  // toggle the menu overlay on lv_layer_top()
}  // namespace knob_menu_overlay

// Global "controlled" frame overlay: a per-controller colored border (stacked
// when several controllers are active) on lv_layer_top(), shown while the
// device is under remote control. Built once at boot on the UI/LVGL task;
// set_sessions() is called from the UI task only (it dirty-compares so it
// repaints only on change). See src/ui/control_frame.cpp.
namespace control_frame {
// Build the stacked borders + name-pill as children of lv_layer_top(). The
// `parent` argument is ignored (kept for the build() convention); the overlay
// always attaches to lv_layer_top(). All elements start hidden.
lv_obj_t *build(lv_obj_t *parent);
// Update visibility/colors for the active sessions (most-recent first, i.e.
// s[0] is the outermost border + the pill). UI-task only.
void set_sessions(const proto::Session *s, int count);
}  // namespace control_frame

}  // namespace ui
