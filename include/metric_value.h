#pragma once

// Single source of truth for resolving a MetricSource to its display-unit
// scalar and to a 0..1 gauge/bar fill fraction.
//
// Historically this lived as two file-static switches in src/ui/ui_layouts.cpp
// (metric_scalar + scalar_unit_fraction), so the MIDL renderer, the legacy
// template renderer, and the manager-pushed widget_registry could not share it
// without dragging in LVGL. This header is dependency-free (units.h +
// boat_data.h + the MetricSource enum) so it is host-testable and reusable by
// every renderer.

#include "boat_data.h"
#include "ui_layouts_types.h"  // ui::layouts::MetricSource

namespace ui::layouts {

// Resolve a MetricSource to its display-unit scalar:
//   speeds -> knots, angles -> degrees [0,360) (Rudder_deg signed),
//   depth -> m, DTW -> nm, water temp -> C, battery SOC -> %, voltage -> V.
// Returns NaN when the underlying field is absent, or when the source is
// non-scalar (None / Position / APState).
double metric_value(MetricSource src, const boat::View &d);

// Map a display value `v` (as returned by metric_value) to a 0..1 gauge/bar
// fill fraction using the built-in per-source heuristic range. Returns NaN for
// sources without an obvious range (headings, positions) or NaN input.
double metric_unit_fraction(MetricSource src, double v);

}  // namespace ui::layouts
