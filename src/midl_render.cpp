// midl_render.cpp — MIDL element -> MetricBinding mapper (Task 1, host-testable).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_render.h"
#include "layout_renderer.h"  // ui::layout_render::path_to_source

#include <stdlib.h>
#include <string.h>

namespace midl::render {

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

// NOTE: This map is SEPARATE from ui::layout_render::widget_to_kind (metric_source_map.cpp).
// It handles MIDL lowercase tokens ("single-value","windrose") emitted by the MIDL editor;
// widget_to_kind handles the legacy editor camelCase tokens ("numeric","windRose").
// Do not merge these two maps.
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

JsonVariantConst select_screen(JsonVariantConst doc, const char *screen_id, const char **out_id) {
    if (out_id) *out_id = nullptr;

    // MIDL `screens` is a JSON ARRAY (schema: screens.type == "array").
    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    if (screens.isNull() || screens.size() == 0) return JsonVariantConst();

    // Match the requested id against each screen's "id".
    if (screen_id && screen_id[0]) {
        for (JsonVariantConst s : screens) {
            if (s.is<JsonObjectConst>() && strcmp(s["id"] | "", screen_id) == 0) {
                if (out_id) *out_id = s["id"] | (const char *)nullptr;
                return s;
            }
        }
    }

    // Fallback: first object screen (id null/empty/not found).
    for (JsonVariantConst s : screens) {
        if (s.is<JsonObjectConst>()) {
            if (out_id) *out_id = s["id"] | (const char *)nullptr;
            return s;
        }
    }

    return JsonVariantConst();
}

JsonVariantConst find_element(JsonVariantConst elements_obj, const char *element_id) {
    if (!element_id) return JsonVariantConst();
    JsonObjectConst elements = elements_obj.as<JsonObjectConst>();
    if (elements.isNull()) return JsonVariantConst();
    // Explicit strcmp iteration — NOT operator[], which can miss `element_id`
    // via ArduinoJson's pointer-identity fast path when it is a separate buffer.
    for (JsonPairConst kv : elements) {
        if (strcmp(kv.key().c_str(), element_id) == 0) return kv.value();
    }
    return JsonVariantConst();
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
    // style.color may be "#rrggbb" hex string (MIDL editor) or an integer; default 0
    // (painter uses theme default).
    out.accent = 0;
    JsonVariantConst col = el["style"]["color"];
    if (col.is<unsigned>()) {
        out.accent = col.as<unsigned>();
    } else if (col.is<const char *>()) {
        const char *cs = col.as<const char *>();
        // Accept only "#" followed by exactly 6 hex digits; reject malformed strings.
        if (cs && cs[0] == '#' && strlen(cs) == 7) {
            char *end = nullptr;
            unsigned long v = strtoul(cs + 1, &end, 16);
            if (end == cs + 7) out.accent = (uint32_t)v;
        }
    }
    return true;
}

}  // namespace midl::render
