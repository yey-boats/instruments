#pragma once

// Spec 19 D4 - widget registry. Given a validated WidgetDef from
// manager_config::parse(), construct the appropriate LVGL widget
// tree under `parent` and refresh it on demand from boat::View.
//
// Implements numeric, text, bar, gauge, compass, windrose, trend, button,
// and autopilot. Scalar values resolve through the shared display-unit
// resolver (metric_value.h) when the path maps to a known MetricSource
// (so SOG shows knots, headings show degrees, etc.), falling back to the
// raw boat::View field for alias/dynamic paths.
//
// NOTE: this is the legacy Spec-19 manager-push renderer, active only in
// non-YEYBOATS_MIDL_ONLY builds. The MIDL renderer (midl_render.cpp +
// ui_layouts painters) is the go-forward path and supersedes this module at
// the T9 cutover; both now share the value resolver in metric_value.h.

#include <lvgl.h>

#include "manager_config.h"
#include "signalk_parser.h"
#include "ui_layouts_types.h"  // ui::layouts::MetricSource

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
    lv_obj_t *root;                 // container
    lv_obj_t *value_lbl;            // primary value (numeric/text/gauge/compass/wind/AP)
    lv_obj_t *title_lbl;            // small caption above value
    lv_obj_t *bar;                  // lv_bar for bar widget
    lv_obj_t *arc;                  // lv_arc for gauge / bezel ring for compass+windrose
    lv_obj_t *sub_lbl;              // secondary line (cardinal / port-stbd / AP target)
    lv_obj_t *chart;                // lv_chart for trend sparkline
    lv_chart_series_t *series;      // trend series
    ui::layouts::MetricSource src;  // resolved once from `path` at create
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
