#pragma once

#include <lvgl.h>

#include "proto/proto.h"  // proto::Session for the control-frame overlay

// System UI screens (docs/midl/midl-only.md): settings, Wi-Fi setup, touch
// calibration and touch grid, plus the knob menu and control-frame overlays.
// Content screens are MIDL records (midl_store.h). Every screen module
// exposes build() (one-time UI construction, returns the root object) and
// refresh() (called from the global 5 Hz tick when the screen is visible).

namespace ui {

namespace touch_cal_screen {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace touch_cal_screen

namespace touch_grid_screen {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace touch_grid_screen

namespace wifi_setup {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace wifi_setup

namespace settings {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
}  // namespace settings

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
