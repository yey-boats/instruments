#pragma once

// SignalK path -> compiled MetricSource bridge used by the MIDL render
// mapping (midl_render.cpp). Pure string-to-enum, host-testable.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "ui_layouts_types.h"

namespace ui::metric_map {

// Map a SignalK path string to the device's compiled MetricSource.
// Returns MetricSource::None for unknown paths; the MIDL mapper then keeps
// the raw path for the dynamic PathStore.
ui::layouts::MetricSource path_to_source(const char *sk_path);

}  // namespace ui::metric_map
