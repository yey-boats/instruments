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
#include "midl_limits.h"       // midl::FirmwareLimits::path_len (StyleAlloc path pool)
#include "ui_layouts_types.h"  // ui::layouts::MetricBinding, WidgetKind, MetricSource

namespace midl::render {

// Caller-owned storage pool for the variable-size style artifacts one screen's
// elements can author: dial markers (element.markers[]), dial sectors
// (style.sectors) and the dynamic-path strings their `dir` bindings / bound
// sector edges may need. map_element bump-allocates from it; the MetricBinding
// stores non-owning pointers into it (same lifetime contract as id/label/unit).
// A null/exhausted pool degrades gracefully: markers/sectors past the pool are
// dropped, dynamic paths past the pool fall back to "hidden" (NaN), never
// dangling.
struct StyleAlloc {
    ui::layouts::DialMarker *markers;  // pool of marker slots
    uint8_t marker_cap;                // total slots in `markers`
    uint8_t markers_used;              // bump cursor (caller resets per screen)
    ui::layouts::DialSector *sectors;  // pool of sector slots
    uint8_t sector_cap;
    uint8_t sectors_used;
    // Pool of raw-path buffers, each midl::FirmwareLimits::path_len bytes.
    char (*paths)[midl::FirmwareLimits::path_len];
    uint8_t path_cap;
    uint8_t paths_used;
};

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
// Dynamic-path fallback (audit item 8): when path_to_source() misses,
// `path_buf`/`dir_buf` (each >= FirmwareLimits::path_len, may be null) receive
// the RAW SignalK path and out.path/out.dir_path point at them, so the widget
// resolves through the dynamic PathStore instead of dropping to None + "--".
// Also mapped: bindings.dir -> out.dir_source/dir_path (dial pointer),
// format.decimals (alias of precision, decimals wins), format.side -> out.side,
// style.size role (S/M/L/XL/Fill) -> out.size_role (1..5), style.center ->
// out.center_bar, style.zones -> out.zones/zone_count (theme tokens + #rrggbb).
//
// `zoom` field (tap-to-fullscreen): absent -> out.zoomable defaults to true for
// any non-Button tile with a real source (out.zoom_target == nullptr, i.e.
// fullscreen-self); boolean false -> out.zoomable = false; a string -> out.zoomable
// = true with out.zoom_target = the copied screen id (switch to that screen).
// `zoom_buf` may be null (zoom string then ignored). Returns false if `el` is not
// an object.
//
// Dial-fidelity wave (all optional trailing params; null = feature degrades):
//   - bindings.value {kind:"const"|"local"} -> out.value_kind + const_value /
//     const_text / local_id (const_text and local_id share `path_buf` — they are
//     mutually exclusive with a dynamic path). Neither subscribes any SignalK
//     path (collect_paths sees source == None, path == NULL).
//   - action.value -> JSON-encoded into `action_value_buf` (>=32) and pointed
//     at by out.action_value (number "42", bool "true", string "\"s\"").
//   - element.markers[] (compass/windrose, cap MAX_DIAL_MARKERS) -> slots from
//     `style_alloc` (out.markers/marker_count); marker dir paths + bound sector
//     edge paths draw from the same pool's path buffers.
//   - style.sectors (cap MAX_DIAL_SECTORS per dial) -> slots from `style_alloc`
//     (out.sectors/sector_count); an edge is a fixed numeric angle or a string
//     naming a `bindings` key for a data-bound edge (dynamic laylines).
//   - style.hull -> out.hull; style.shape "band" -> out.shape = DialShape::Band.
bool map_element(JsonVariantConst el, const char *element_id, ui::layouts::MetricBinding &out,
                 char *id_buf, char *label_buf, char *unit_buf, char *action_buf, char *zoom_buf,
                 char *path_buf = nullptr, char *dir_buf = nullptr,
                 char *action_value_buf = nullptr, StyleAlloc *style_alloc = nullptr);

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

// Dismiss the transient MIDL fullscreen-zoom screen (__zoom__) if it is the
// currently-shown screen, returning to the screen it was launched from (the
// captured return id, falling back to screen 0). Returns true if a zoom was
// active and dismissed, false otherwise (caller then handles the gesture
// normally). Used by the touch-task swipe detector so ANY swipe on the zoom
// view returns rather than navigating to a sibling screen (the zoom is a
// hidden registered screen, so next()/prev() would otherwise skip past it).
// Device-only (implemented in midl_render_apply.cpp); runs on the UI task.
bool dismiss_zoom();

}  // namespace midl::render
