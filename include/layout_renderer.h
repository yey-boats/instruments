#pragma once

// Layout renderer: bridges a parsed `layout::Screen` (editor JSON shape
// with `widget` + `primary`/`secondary` SK paths per tile) to a live
// `ui::layouts::ScreenVariantSpec` + `MetricBinding[]` that the
// QuadGrid template knows how to paint. Each layout-defined screen
// gets one renderer slot whose MetricBinding table lives for the
// session.
//
// Call `apply()` after `layout::apply_json()` succeeds. It walks the
// loaded layout::Config and for every screen whose id matches a
// registered ui::Screen, calls ui::replace_screen() with a freshly
// built root from the editor's widget+path spec. Screens not in the
// loaded layout keep their hardcoded MetricBinding tables.

#include "layout.h"
#include "ui_layouts_types.h"

namespace ui::layout_render {

// Map a layout editor widget string ("numeric", "compass", "gauge",
// "bar", "windRose", "autopilot", "text", "button", "trend") to a
// device WidgetKind. Unknown widget strings fall back to Numeric.
ui::layouts::WidgetKind widget_to_kind(const char *widget);

// Map a SignalK path string to the device's compiled MetricSource.
// Returns MetricSource::None for unknown paths; the renderer then
// renders "--" for that tile but the chrome still paints.
ui::layouts::MetricSource path_to_source(const char *sk_path);

// Apply the currently-loaded layout::Config to the screen manager:
// for each layout::Screen whose id has a corresponding registered
// ui::Screen, build a new root from the layout's tiles and swap it
// in via ui::replace_screen(). Returns the number of screens replaced.
size_t apply();

}  // namespace ui::layout_render
