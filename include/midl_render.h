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

namespace midl::render {

// Map a MIDL element token ("single-value","text","gauge","bar","compass",
// "windrose","trend","autopilot","button") to a device WidgetKind. Unknown
// tokens fall back to Numeric (matches the legacy renderer's policy).
ui::layouts::WidgetKind token_to_kind(const char *type);

// Pure: fill `out` from one MIDL element JSON. Strings (id,label,unit) are
// COPIED into caller-owned buffers `id_buf`/`label_buf`/`unit_buf` (each >=32),
// and out.id/label/unit point at them -- so the caller controls lifetime (the
// MetricBinding stores non-owning pointers). `value` binding path -> source via
// path_to_source; style.color -> accent (accepts "#rrggbb" hex or integer).
// Returns false if `el` is not an object.
bool map_element(JsonVariantConst el, const char *element_id, ui::layouts::MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf);

// Orchestration entry point (device-only; implemented in midl_render_apply.cpp
// which is NOT in the native build_src_filter). Runs ON THE UI TASK — caller
// guarantees this (e.g. inside app::pump()).
//
// Finds `screen_id` (or the first screen if null/missing) in doc["screens"],
// solves its layout, maps each element to a MetricBinding via the enum bridge,
// builds a freeform LVGL screen with create_freeform(), and registers it via
// ui::replace_screen / ui::register_screen.
//
// SINGLE-SCREEN LIMITATION (Slice 2): only one MIDL screen is live at a time.
// The session arena is a single function-static block; a second apply_doc call
// overwrites it. Multi-screen support is deferred to Slice 5 (cutover).
//
// Returns true if the screen was successfully built and registered.
bool apply_doc(JsonVariantConst doc, const char *screen_id);

}  // namespace midl::render
