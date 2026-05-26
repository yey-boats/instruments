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

namespace wind {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void set_refresh_enabled(bool e);
bool refresh_enabled();
}  // namespace wind

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

}  // namespace ui
