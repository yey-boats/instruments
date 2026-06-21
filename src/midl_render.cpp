// midl_render.cpp — MIDL element -> MetricBinding mapper (Task 1, host-testable).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_render.h"
#include "layout_renderer.h"  // ui::layout_render::path_to_source

#include <string.h>

namespace midl {
namespace render {

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

WidgetKind token_to_kind(const char *t) {
    if (!t) return WidgetKind::Numeric;
    if (!strcmp(t, "compass")) return WidgetKind::Compass;
    if (!strcmp(t, "windrose")) return WidgetKind::WindRose;
    if (!strcmp(t, "gauge")) return WidgetKind::Gauge;
    if (!strcmp(t, "bar")) return WidgetKind::Bar;
    if (!strcmp(t, "autopilot")) return WidgetKind::Autopilot;
    if (!strcmp(t, "text")) return WidgetKind::Text;
    if (!strcmp(t, "button")) return WidgetKind::Button;
    if (!strcmp(t, "trend")) return WidgetKind::Trend;
    return WidgetKind::Numeric;  // "single-value" + unknown
}

static void copy32(char *dst, const char *src) {
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, 31);
    dst[31] = 0;
}

bool map_element(JsonVariantConst el, const char *element_id, MetricBinding &out, char *id_buf,
                 char *label_buf, char *unit_buf) {
    if (!el.is<JsonObjectConst>()) return false;
    memset(&out, 0, sizeof(out));
    copy32(id_buf, element_id);
    const char *name = el["name"] | element_id;
    copy32(label_buf, name);
    copy32(unit_buf, el["format"]["unit"] | "");
    out.id = id_buf;
    out.label = label_buf;
    out.unit = unit_buf;
    out.kind = token_to_kind(el["type"] | "");
    out.source = ui::layout_render::path_to_source(el["bindings"]["value"]["path"] | "");
    // style.color may be "#rrggbb" or an int; default 0 (painter uses theme default).
    out.accent = 0;
    JsonVariantConst col = el["style"]["color"];
    if (col.is<unsigned>()) out.accent = col.as<unsigned>();
    return true;
}

}  // namespace render
}  // namespace midl
