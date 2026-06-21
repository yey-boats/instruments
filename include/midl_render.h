#pragma once

// MIDL firmware render port — public API.
//
// Task 1 (this slice): pure element -> MetricBinding mapper (host-testable).
// Tasks 2-4: freeform LVGL builder + orchestration + app-event (device).
//
// Host-clean: includes only ui_layouts_types.h (no <lvgl.h>) and ArduinoJson.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <ArduinoJson.h>
#include "ui_layouts_types.h"  // ui::layouts::MetricBinding, WidgetKind, MetricSource

namespace midl {
namespace render {

// Map a MIDL element token ("single-value","text","gauge","bar","compass",
// "windrose","trend","autopilot","button") to a device WidgetKind. Unknown
// tokens fall back to Numeric (matches the legacy renderer's policy).
ui::layouts::WidgetKind token_to_kind(const char *type);

// Pure: fill `out` from one MIDL element JSON. Strings (id,label,unit) are
// COPIED into caller-owned buffers `id_buf`/`label_buf`/`unit_buf` (each >=32),
// and out.id/label/unit point at them -- so the caller controls lifetime (the
// MetricBinding stores non-owning pointers). `value` binding path -> source via
// path_to_source; style.color -> accent. Returns false if `el` is not an object.
bool map_element(JsonVariantConst el, const char *element_id, ui::layouts::MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf);

}  // namespace render
}  // namespace midl
