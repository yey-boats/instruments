#pragma once

// Spec 19 D4 - widget registry. Given a validated WidgetDef from
// manager_config::parse(), construct the appropriate LVGL widget
// tree under `parent` and refresh it on demand from boat::View.
//
// MVP implements numeric, text, bar fully; gauge, compass, trend,
// button, autopilot are stubbed (logs + placeholder label) so D5
// variant apply can wire them without crashing.

#include <lvgl.h>

#include "manager_config.h"
#include "signalk_parser.h"

namespace widget_registry {

// Per-widget runtime handle. Built by create(); fed to update() on
// each refresh.
struct Widget {
    manager_config::WidgetType type;
    char id[manager_config::MAX_WIDGET_ID + 1];
    char path[manager_config::MAX_PATH + 1];
    char unit[manager_config::MAX_UNIT + 1];
    uint8_t precision;
    double min;
    double max;
    lv_obj_t *root;       // container
    lv_obj_t *value_lbl;  // primary value (numeric/text)
    lv_obj_t *title_lbl;  // small caption above value
    lv_obj_t *bar;        // lv_bar for bar widget
};

// Build a widget under `parent` at (x, y) sized w x h. Returns nullptr
// on alloc failure. Widget remains usable until parent is destroyed.
Widget *create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
               const manager_config::WidgetDef &def, const manager_config::WidgetStyle &defaults);

// Free the small runtime handle allocated by create(). The LVGL object tree is
// owned by the parent/root and should be deleted separately.
void destroy(Widget *w);

// Re-read the widget's data path and update the LVGL tree.
void update(Widget &w, const boat::View &data);

}  // namespace widget_registry
