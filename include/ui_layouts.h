#pragma once

// MIDL tile painters (the device half of the MIDL render path).
//
// midl_render_apply.cpp maps each MIDL element to a MetricBinding and the
// solver's pixel rect, then builds the screen with create_freeform() and
// refreshes it with update_freeform(). The per-kind painters (numeric,
// compass, gauge, bar, windrose + the winddial / windsteer / aphud HUD
// composites, text, button, autopilot, trend, clinometer) live in
// ui_layouts.cpp.
//
// POD types (MetricSource, MetricRow, WidgetKind, MetricBinding,
// ScreenVariantSpec) are defined in ui_layouts_types.h so that host-only TUs
// (midl_render, tests) can include the enums without pulling in <lvgl.h>.

#include <lvgl.h>
#include <stdint.h>
#include "signalk_parser.h"
#include "ui_layouts_types.h"
#include "trend_backfill.h"  // trend::backfill::Result (pure header)

namespace sk {
class SubscriptionSet;
}

namespace ui::layouts {

// Fullscreen-zoom handler hook, set by the MIDL render layer: a tap on a
// fullscreen-self zoomable tile (zoom_target == nullptr / "auto"/"") builds a
// transient single-element full-screen render of the tapped metric. NULL (the
// default) makes such a tap a no-op. Runs on the UI/LVGL task.
typedef void (*ZoomFullscreenFn)(const MetricBinding &m);
void set_zoom_fullscreen_handler(ZoomFullscreenFn fn);

// Reverse of ui::metric_map::path_to_source(): the canonical SignalK path a
// MetricSource consumes, or nullptr for non-path sources (None). Position and
// APState ARE real paths a screen subscribes (navigation.position /
// steering.autopilot.state) and so map to their strings. Used by the per-screen
// subscription manager (collect_paths) and the perf-counter store shadow.
const char *source_to_path(MetricSource s);

// Collect every SignalK path the screen `spec` consumes (primary sources plus
// every extras[] row) into `out`. `out` is NOT cleared first (so callers can
// accumulate the baseline + a screen). Non-path sources are skipped. Pure /
// host-testable; takes no LVGL or live state.
void collect_paths(const ScreenVariantSpec &spec, sk::SubscriptionSet &out);

// Freeform layout: one tile per (MetricBinding, Rect) pair, placed at the
// caller's solved pixel rect. `spec.metrics` has `spec.metric_count` entries;
// `rects[i]` is the pixel rect for metric i (x, y, w, h).
lv_obj_t *create_freeform(lv_obj_t *parent, const ScreenVariantSpec &spec, const Rect *rects);
void update_freeform(lv_obj_t *root, const ScreenVariantSpec &spec, const boat::View &data);

// Trend history backfill + clock, injected by the MIDL render layer (device:
// trend::backfill, src/trend_backfill.cpp). NULL (host/sim builds) records live
// samples only, against the LVGL tick.
struct TrendBackfillOps {
    double (*now_s)();
    uint32_t (*begin)();
    bool (*enqueue)(uint32_t gen, uint8_t series, const char *path, trend::Agg agg,
                    uint32_t window_s, double step_s);
    bool (*poll)(uint32_t gen, trend::backfill::Result &out);
    void (*done)();
};
void set_trend_backfill(const TrendBackfillOps *ops);

// The active screen changed (UI task): drops the trend series of a screen that
// is no longer shown. History records only for the screen on display.
void trend_screen_changed();

// Trend history footprint for the perf/heap log: series in use and bytes held.
void trend_stats(int &series, size_t &bytes);

}  // namespace ui::layouts
