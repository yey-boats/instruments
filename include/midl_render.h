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

// Pure: fill `out` from one MIDL element JSON. Strings (id,label,unit,action,zoom)
// are COPIED into caller-owned buffers `id_buf`/`label_buf`/`unit_buf`/
// `action_buf`/`zoom_buf` (each >=32), and out.id/label/unit +
// out.target_screen/out.command + out.zoom_target point at them -- so the caller
// controls lifetime (the MetricBinding stores non-owning pointers). `value`
// binding path -> source via path_to_source; style.color -> accent (accepts
// "#rrggbb" hex or integer). A MIDL `action` block maps by kind: "nav" ->
// out.target_screen, "command" -> out.command (both copied into action_buf; only
// one is set).
//
// `zoom` field (tap-to-fullscreen): absent -> out.zoomable defaults to true for
// any non-Button tile with a real source (out.zoom_target == nullptr, i.e.
// fullscreen-self); boolean false -> out.zoomable = false; a string -> out.zoomable
// = true with out.zoom_target = the copied screen id (switch to that screen).
// `zoom_buf` may be null (zoom string then ignored). Returns false if `el` is not
// an object.
bool map_element(JsonVariantConst el, const char *element_id, ui::layouts::MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf, char *action_buf, char *zoom_buf);

// Pure (host-testable): select a screen object from a MIDL document.
//
// MIDL `screens` is a JSON ARRAY of screen objects, each with an "id" field
// (per yb-midl-config.schema.json: screens.type == "array"). This walks
// doc["screens"] and returns the object whose "id" matches `screen_id`. If
// `screen_id` is null/empty/unmatched, returns the FIRST object screen.
//
// Writes the chosen screen's "id" field to *out_id (may be null). Returns a
// null JsonVariantConst if `screens` is missing/empty or has no object screen.
//
// NOTE: the array form is load-bearing — an earlier `doc["screens"].is<
// JsonObjectConst>()` check rejected the (correct) array shape and rendered
// nothing. Keep the array handling.
JsonVariantConst select_screen(JsonVariantConst doc, const char *screen_id, const char **out_id);

// Pure (host-testable): find an element in a MIDL elements object by KEY.
//
// `elements_obj` is the screen's "elements" map ({ "<id>": {...}, ... }).
// Returns the value whose KEY equals `element_id`, found by explicit strcmp
// iteration over the object's pairs — NOT operator[], which can miss a key
// when `element_id` is a separately-allocated buffer (ArduinoJson's
// pointer-identity fast path; observed in the headless sim). Returns a null
// JsonVariantConst if not found or if `elements_obj` is not an object.
JsonVariantConst find_element(JsonVariantConst elements_obj, const char *element_id);

// Orchestration entry points (device-only; implemented in midl_render_apply.cpp
// which is NOT in the native build_src_filter). Both run ON THE UI TASK —
// caller guarantees this (e.g. inside setup() or app::pump()).

// apply_all: register EVERY screen in doc["screens"] (a JSON array), up to
// ui::MAX_SCREENS. Each screen is solved, its elements mapped to MetricBindings
// via the enum bridge, built into a freeform LVGL screen, and registered with
// ui::register_screen / ui::replace_screen. Per-screen arenas live in a
// PSRAM-allocated pool; per-slot refresh/collect-paths trampolines drive the 5 Hz
// refresh and subscription manager. After building, the default screen is shown
// (doc settings.defaultScreen / doc.defaultScreen if present, else the first
// registered screen). Navigation between the registered screens then works via
// the standard `screen <id|next|prev>` / ui::show_by_id path.
//
// Returns the number of screens successfully built.
size_t apply_all(JsonVariantConst doc);

// apply_doc: single-screen convenience. Builds ALL screens in the doc via
// apply_all (so navigation works regardless of entry point), then shows
// `screen_id` (null/empty/unmatched -> apply_all's default screen governs which
// is visible). Used by the `midl-render` console command and the
// ConfigApplyMidl pump case.
//
// Returns true if at least one screen was built and registered.
bool apply_doc(JsonVariantConst doc, const char *screen_id);

}  // namespace midl::render
