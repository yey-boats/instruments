#pragma once

#include <lvgl.h>

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

}  // namespace ui
