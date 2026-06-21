#pragma once

// Layout templates per docs/specs/11-layout-templates-screen-variants.md.
//
// A screen becomes a variant of a small template catalog:
//
//   screen = template + metric bindings + tap commands
//
// This file declares the template ids, metric binding shape, and the
// factory entry points. Each template lives in ui_layouts.cpp (or
// later, in dedicated layout_<name>.cpp files).
//
// Phase 1 lands the framework + the `quad_grid` template (2x2 tile
// dashboard). Other templates are stubbed and registered as they're
// implemented; un-implemented ones return NULL so callers can decide
// whether to fall back to a hand-built screen.
//
// POD types (TemplateId, MetricSource, MetricRow, WidgetKind,
// MetricBinding, ScreenVariantSpec) are defined in ui_layouts_types.h
// so that host-only TUs (midl_render, tests) can include the enums
// without pulling in <lvgl.h>.

#include <lvgl.h>
#include <stdint.h>
#include "signalk_parser.h"
#include "ui_layouts_types.h"

namespace sk {
class SubscriptionSet;
}

namespace ui::layouts {

// Build the screen widget tree for `spec`. Returns NULL if the
// template isn't implemented yet. Caller passes the returned root to
// ui::register_screen.
lv_obj_t *create(lv_obj_t *parent, const ScreenVariantSpec &spec);

// Update displayed values from a snapshot. Idempotent and uses the
// dirty-value caches in include/ui_dirty.h so the partial-render path
// stays cold when nothing changed.
void update(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data);

// Drive the full-screen "zoom" view to a chosen metric (normally set by a tile
// tap; also used by the sim harness and remote controllers).
void set_zoom_target(const MetricBinding &m);

// Reverse of layout_renderer's path_to_source(): the canonical SignalK path a
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
// caller's solved pixel rect. Reuses the per-tile painters from the QuadGrid
// template. `spec.metrics` has `spec.metric_count` entries; `rects[i]` is the
// pixel rect for metric i (x, y, w, h). Called directly by midl_render — NOT
// routed through create() — so no TemplateId enum churn is needed.
lv_obj_t *create_freeform(lv_obj_t *parent, const ScreenVariantSpec &spec, const Rect *rects);
void update_freeform(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data);

}  // namespace ui::layouts
