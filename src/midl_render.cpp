// midl_render.cpp — MIDL element -> MetricBinding mapper (Task 1, host-testable).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_render.h"
#include "layout_renderer.h"  // ui::layout_render::path_to_source
#include "midl_limits.h"      // midl::FirmwareLimits::path_len

#include <stdlib.h>
#include <string.h>

namespace midl::render {

using ui::layouts::BindKind;
using ui::layouts::DialMarker;
using ui::layouts::DialSector;
using ui::layouts::DialShape;
using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;
using ui::layouts::ZoneColor;

// NOTE: This map is SEPARATE from ui::layout_render::widget_to_kind (metric_source_map.cpp).
// It handles MIDL lowercase tokens ("single-value","windrose") emitted by the MIDL editor;
// widget_to_kind handles the legacy editor camelCase tokens ("numeric","windRose").
// Do not merge these two maps.
WidgetKind token_to_kind(const char *t) {
    if (!t) return WidgetKind::Numeric;
    if (!strcmp(t, "compass")) return WidgetKind::Compass;
    if (!strcmp(t, "windrose")) return WidgetKind::WindRose;
    if (!strcmp(t, "windsteer")) return WidgetKind::WindSteer;
    // Firmware extension token — "clinometer" is NOT in the midl catalog yet
    // (upstream follow-up); the schema allows unknown types with a warning.
    if (!strcmp(t, "clinometer")) return WidgetKind::Clinometer;
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

// Copy a raw SignalK path into a caller-owned FirmwareLimits::path_len buffer.
static void copy_path(char *dst, const char *src) {
    constexpr size_t cap = midl::FirmwareLimits::path_len;
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

// Bump-allocate one raw-path buffer from the StyleAlloc pool and copy `src`
// into it. Returns NULL when the pool is absent/exhausted (caller degrades).
static const char *alloc_style_path(StyleAlloc *sa, const char *src) {
    if (!sa || !sa->paths || sa->paths_used >= sa->path_cap || !src || !src[0]) return nullptr;
    char *dst = sa->paths[sa->paths_used++];
    copy_path(dst, src);
    return dst;
}

// Parse "#rrggbb" into rgb; returns true on a well-formed literal.
static bool parse_hex_color(const char *cs, uint32_t &rgb) {
    if (!cs || cs[0] != '#' || strlen(cs) != 7) return false;
    char *end = nullptr;
    unsigned long v = strtoul(cs + 1, &end, 16);
    if (end != cs + 7) return false;
    rgb = (uint32_t)v;
    return true;
}

// Map a MIDL zone/marker colour string (theme token or "#rrggbb") to the
// palette-independent ZoneColor encoding the painter resolves at draw time.
static ZoneColor parse_zone_color(const char *cs, uint32_t &rgb) {
    rgb = 0;
    if (!cs || !cs[0]) return ZoneColor::Default;
    if (cs[0] == '#') return parse_hex_color(cs, rgb) ? ZoneColor::Literal : ZoneColor::Default;
    if (!strcmp(cs, "accent")) return ZoneColor::Accent;
    if (!strcmp(cs, "warn")) return ZoneColor::Warn;
    if (!strcmp(cs, "alarm") || !strcmp(cs, "bad") || !strcmp(cs, "danger"))
        return ZoneColor::Alarm;
    if (!strcmp(cs, "good")) return ZoneColor::Good;
    if (!strcmp(cs, "port")) return ZoneColor::Port;
    if (!strcmp(cs, "starboard")) return ZoneColor::Starboard;
    return ZoneColor::Default;
}

// Parse one sector edge (style.sectors[].from/.to). A number is a fixed angle
// in degrees; a string names a key in the element's `bindings` map (dynamic
// laylines per midl types.ts:57) resolved through the enum bridge, a retained
// raw path, or a const binding. Returns false when the edge is unusable (the
// whole sector is then skipped, mirroring the web's numeric-only strictness).
static bool parse_sector_edge(JsonVariantConst edge, JsonVariantConst bindings, StyleAlloc *sa,
                              float &fixed, MetricSource &src, const char *&path) {
    fixed = NAN;
    src = MetricSource::None;
    path = nullptr;
    if (edge.is<float>()) {
        fixed = edge.as<float>();
        return true;
    }
    if (!edge.is<const char *>()) return false;
    // `edge` names a bindings key; the key string lives in the same document,
    // so ArduinoJson's operator[] pointer-identity fast path is safe here.
    JsonVariantConst b = bindings[edge.as<const char *>()];
    if (!b.is<JsonObjectConst>()) return false;
    if (!strcmp(b["kind"] | "", "const")) {
        JsonVariantConst cv = b["value"];
        if (!cv.is<float>()) return false;
        fixed = cv.as<float>();
        return true;
    }
    const char *p = b["path"] | "";
    if (!p[0]) return false;
    src = ui::layout_render::path_to_source(p);
    if (src != MetricSource::None) return true;
    path = alloc_style_path(sa, p);
    return path != nullptr;
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
                 char *label_buf, char *unit_buf, char *action_buf, char *zoom_buf, char *path_buf,
                 char *dir_buf, char *action_value_buf, StyleAlloc *style_alloc) {
    if (!el.is<JsonObjectConst>()) return false;
    memset(&out, 0, sizeof(out));
    if (action_buf) action_buf[0] = 0;
    if (zoom_buf) zoom_buf[0] = 0;
    if (path_buf) path_buf[0] = 0;
    if (dir_buf) dir_buf[0] = 0;
    if (action_value_buf) action_value_buf[0] = 0;
    copy32(id_buf, element_id);
    const char *name = el["name"] | element_id;
    copy32(label_buf, name);
    copy32(unit_buf, el["format"]["unit"] | "");
    out.id = id_buf;
    out.label = label_buf;
    out.unit = unit_buf;
    out.kind = token_to_kind(el["type"] | "");
    // bindings.value: dispatch on Source.kind (schema $defs/source). "signalk"
    // (or an absent kind with a path — legacy docs) takes the enum bridge; on a
    // miss the RAW path is RETAINED (audit item 8) for the dynamic PathStore.
    // "const" renders a literal and "local" a device-local metric — NEITHER
    // subscribes anything (source stays None, path stays NULL, so collect_paths
    // adds no SignalK subscription for them). "computed" is unsupported -> "--".
    JsonVariantConst vbind = el["bindings"]["value"];
    const char *vkind = vbind["kind"] | "";
    if (!strcmp(vkind, "const")) {
        JsonVariantConst cv = vbind["value"];
        if (cv.is<bool>()) {
            out.value_kind = BindKind::ConstBind;
            out.const_value = cv.as<bool>() ? 1.0f : 0.0f;
        } else if (cv.is<float>()) {
            out.value_kind = BindKind::ConstBind;
            out.const_value = cv.as<float>();
        } else if (cv.is<const char *>() && path_buf) {
            out.value_kind = BindKind::ConstBind;
            out.const_value = NAN;
            copy_path(path_buf, cv.as<const char *>());
            out.const_text = path_buf;
        }
        // missing/unusable literal: value_kind stays PathBind + source None -> "--"
    } else if (!strcmp(vkind, "local")) {
        const char *lid = vbind["id"] | "";
        if (lid[0] && path_buf) {
            out.value_kind = BindKind::LocalBind;
            copy_path(path_buf, lid);
            out.local_id = path_buf;
        }
    } else {
        const char *vpath = vbind["path"] | "";
        out.source = ui::layout_render::path_to_source(vpath);
        if (out.source == MetricSource::None && vpath[0] && path_buf) {
            copy_path(path_buf, vpath);
            out.path = path_buf;
        }
    }
    // bindings.dir (audit item 3): a second source steering the dial pointer
    // (compass/windrose) independently of the centre value — midl types.ts:24;
    // the web dial renders it as the dashed warn pointer (dirDeg).
    const char *dpath = el["bindings"]["dir"]["path"] | "";
    out.dir_source = ui::layout_render::path_to_source(dpath);
    if (out.dir_source == MetricSource::None && dpath[0] && dir_buf) {
        copy_path(dir_buf, dpath);
        out.dir_path = dir_buf;
    }
    // Optional per-element scaling/formatting from the MIDL `format` block.
    // Defaults (set by the memset above): range_min==range_max==0 means "painter
    // uses its built-in default range"; precision==-1 means "painter default".
    out.range_min = 0;
    out.range_max = 0;
    out.precision = -1;
    // range is a 2-element [min,max] array. The schema places it under `style`
    // (the canonical library docs author style.range); the editor historically
    // emitted format.range, which wins when both are present (back-compat).
    JsonArrayConst rng = el["format"]["range"].as<JsonArrayConst>();
    if (rng.isNull() || rng.size() != 2) rng = el["style"]["range"].as<JsonArrayConst>();
    if (!rng.isNull() && rng.size() == 2) {
        out.range_min = rng[0] | 0.0f;
        out.range_max = rng[1] | 0.0f;
    }
    // Decimal places: `decimals` is the canonical MIDL key (what the web
    // formatValue reads — types.ts documents `precision` as its alias), so
    // decimals wins when both are present. Only non-negative ints are honored.
    JsonVariantConst prec = el["format"]["decimals"];
    if (!prec.is<int>()) prec = el["format"]["precision"];
    if (prec.is<int>()) {
        int p = prec.as<int>();
        if (p >= 0 && p <= 127) out.precision = (int8_t)p;
    }
    // format.side (audit item 5): truthy = boolean true or a non-empty string
    // (the design's "port-stbd"), mirroring web model.ts sideEnabled().
    JsonVariantConst side = el["format"]["side"];
    out.side = (side.is<bool>() && side.as<bool>()) ||
               (side.is<const char *>() && (side.as<const char *>())[0] != 0);
    // style.size role (audit item 6): S/M/L/XL/Fill -> 1..5 (the painter maps
    // onto the enabled font ladder {14,20,28,48,64}). Legacy numeric px sizes
    // stay 0 = auto (the height-based ladder pick).
    JsonVariantConst sz = el["style"]["size"];
    if (sz.is<const char *>()) {
        const char *s = sz.as<const char *>();
        if (!strcmp(s, "S"))
            out.size_role = 1;
        else if (!strcmp(s, "M"))
            out.size_role = 2;
        else if (!strcmp(s, "L"))
            out.size_role = 3;
        else if (!strcmp(s, "XL"))
            out.size_role = 4;
        else if (!strcmp(s, "Fill"))
            out.size_role = 5;
    }
    // style.center (audit item 7): presence turns a bar into a centre-zero
    // deviation needle (web tiles.ts barSvg keys on `center != null`; the
    // numeric value itself is not used — deviation is around the range middle).
    out.center_bar = el["style"]["center"].is<float>();
    // style.zones (audit item 4): ordered threshold colour bands. Colours are
    // theme tokens or #rrggbb literals; resolution against the live palette
    // happens in the painter (zone_rgb) so day/night flips keep working.
    JsonArrayConst zones = el["style"]["zones"].as<JsonArrayConst>();
    if (!zones.isNull()) {
        for (JsonVariantConst z : zones) {
            if (out.zone_count >= ui::layouts::MAX_METRIC_ZONES) break;
            JsonVariantConst lt = z["lt"];
            if (!lt.is<float>()) continue;  // schema requires a numeric lt
            ui::layouts::MetricZone &mz = out.zones[out.zone_count];
            mz.lt = lt.as<float>();
            mz.color = parse_zone_color(z["color"] | "", mz.rgb);
            ++out.zone_count;
        }
    }
    // style.hull / style.shape (dial presentation, web dial.ts:139-146/:50).
    out.hull = el["style"]["hull"] | false;
    if (!strcmp(el["style"]["shape"] | "", "band")) out.shape = DialShape::Band;
    // style.sectors: colored dial arcs (no-go zone / laylines). Edges are fixed
    // degrees or bindings-key strings (parse_sector_edge); colours reuse the
    // zone token encoding and resolve against the live theme in the painter.
    // Slots come from the caller's StyleAlloc pool (per-SCREEN, like markers —
    // inlining DialSector[3] in every MetricBinding would blow the arena budget).
    if (style_alloc && style_alloc->sectors) {
        JsonArrayConst secs = el["style"]["sectors"].as<JsonArrayConst>();
        JsonVariantConst bindings = el["bindings"];
        if (!secs.isNull()) {
            uint8_t start = style_alloc->sectors_used;
            uint8_t count = 0;
            for (JsonVariantConst s : secs) {
                if (count >= ui::layouts::MAX_DIAL_SECTORS) break;
                if (style_alloc->sectors_used >= style_alloc->sector_cap) break;
                DialSector &ds = style_alloc->sectors[style_alloc->sectors_used];
                if (!parse_sector_edge(s["from"], bindings, style_alloc, ds.from, ds.from_source,
                                       ds.from_path))
                    continue;
                if (!parse_sector_edge(s["to"], bindings, style_alloc, ds.to, ds.to_source,
                                       ds.to_path))
                    continue;
                ds.color = parse_zone_color(s["color"] | "", ds.rgb);
                ++style_alloc->sectors_used;
                ++count;
            }
            if (count) {
                out.sectors = &style_alloc->sectors[start];
                out.sector_count = count;
            }
        }
    }
    // element.markers[]: per-marker glyph / colour / dir binding / rim-vs-vector
    // kind (schema $defs + midl types.ts Marker). Slots come from the caller's
    // StyleAlloc pool; the manifest cap (maxMarkersPerDial == MAX_DIAL_MARKERS)
    // bounds one dial. Only dial kinds render markers, so non-dial elements do
    // not consume pool slots.
    if (style_alloc && style_alloc->markers &&
        (out.kind == WidgetKind::Compass || out.kind == WidgetKind::WindRose)) {
        JsonArrayConst mks = el["markers"].as<JsonArrayConst>();
        if (!mks.isNull()) {
            uint8_t start = style_alloc->markers_used;
            uint8_t count = 0;
            for (JsonVariantConst mk : mks) {
                if (count >= ui::layouts::MAX_DIAL_MARKERS) break;
                if (style_alloc->markers_used >= style_alloc->marker_cap) break;
                DialMarker &dm = style_alloc->markers[style_alloc->markers_used];
                ui::GlyphId g = ui::glyph_from_token(mk["glyph"] | "triangle");
                // Unknown glyph -> Circle (the web's small filled-dot fallback).
                dm.glyph = (uint8_t)(g == ui::GlyphId::COUNT ? ui::GlyphId::Circle : g);
                dm.vector = !strcmp(mk["kind"] | "rim", "vector");
                dm.color = parse_zone_color(mk["color"] | "", dm.rgb);
                dm.dir_source = MetricSource::None;
                dm.dir_path = nullptr;
                dm.dir_const = NAN;
                JsonVariantConst dir = mk["dir"];
                if (!strcmp(dir["kind"] | "", "const")) {
                    JsonVariantConst cv = dir["value"];
                    if (cv.is<float>()) dm.dir_const = cv.as<float>();
                } else {
                    const char *dp = dir["path"] | "";
                    dm.dir_source = ui::layout_render::path_to_source(dp);
                    if (dm.dir_source == MetricSource::None && dp[0])
                        dm.dir_path = alloc_style_path(style_alloc, dp);
                }
                ++style_alloc->markers_used;
                ++count;
            }
            if (count) {
                out.markers = &style_alloc->markers[start];
                out.marker_count = count;
            }
        }
    }
    // style.color may be "#rrggbb" hex string (MIDL editor) or an integer; default 0
    // (painter uses theme default).
    out.accent = 0;
    JsonVariantConst col = el["style"]["color"];
    if (col.is<unsigned>()) {
        out.accent = col.as<unsigned>();
    } else if (col.is<const char *>()) {
        // Accept only "#" followed by exactly 6 hex digits; reject malformed strings.
        uint32_t rgb = 0;
        if (parse_hex_color(col.as<const char *>(), rgb)) out.accent = rgb;
    }
    // Per-element `zoom` (tap-to-fullscreen). Three shapes:
    //   absent      -> DEFAULT: every value/instrument tile is zoomable to its
    //                  own full-screen render (zoom_target == nullptr means
    //                  "fullscreen-self"); Buttons and source-less tiles are not.
    //   boolean      -> false disables zoom; true keeps the fullscreen-self default.
    //   string       -> a screen id to switch to instead of fullscreen-self;
    //                   copied into zoom_buf so the MetricBinding keeps a
    //                   non-owning ptr with caller-controlled lifetime (like
    //                   id/label/unit/action).
    // NOTE: zoom_target == nullptr is the "fullscreen-self / auto-scale" signal
    // the tile tap handler keys on; a non-null "auto"/"" also auto-scales (see
    // layout::zoom_action), and any other string switches to that screen.
    //
    // Interactivity gating: create_freeform() decides whether to wire a tile tap
    // by `zoom_target ? (zoom_action(zoomable,zoom_target) != ZOOM_NONE) : (source
    // != None)`. A LEGACY built-in tile has zoom_target == nullptr and relies on
    // the `source != None` fallback to stay tappable. So a MIDL tile that must NOT
    // zoom (explicit `zoom:false`, or a source-less/Button tile) cannot leave
    // zoom_target == nullptr — that would fall into the legacy `source != None`
    // branch and zoom anyway. We point its zoom_target at the EMPTY zoom_buf
    // instead: zoom_action(zoomable=false, "") == ZOOM_NONE, so the tile is wired
    // non-interactive. The fullscreen-self default/true case keeps nullptr.
    JsonVariantConst zoom = el["zoom"];
    // A dynamic-path binding (source None but path retained) is a real value
    // tile: it zooms and stays tappable exactly like an enum-source tile. So
    // are const/local bindings (they render a real value without subscribing).
    bool has_value = out.source != MetricSource::None || (out.path && out.path[0]) ||
                     out.value_kind != BindKind::PathBind;
    bool self_zoomable = (out.kind != WidgetKind::Button && has_value);
    out.zoomable = self_zoomable;
    out.zoom_target = self_zoomable ? nullptr /* fullscreen-self */
                                    : (zoom_buf ? zoom_buf : nullptr) /* "" -> ZOOM_NONE */;
    if (zoom.is<bool>()) {
        bool want = zoom.as<bool>() && self_zoomable;
        out.zoomable = want;
        out.zoom_target = want ? nullptr : (zoom_buf ? zoom_buf : nullptr);
    } else if (zoom.is<const char *>()) {
        const char *zs = zoom.as<const char *>();
        if (zs && zs[0] && zoom_buf) {
            copy32(zoom_buf, zs);
            out.zoomable = true;
            out.zoom_target = zoom_buf;  // switch to this screen on tap
        }
    }

    // action block (buttons / interactive elements). actionKinds == {nav, command}
    // per the MIDL manifest. "nav" -> target_screen (reuses the tile-nav path);
    // "command" -> command (routed through net::dispatchCommand on tap). The target
    // string is copied into action_buf so the MetricBinding keeps a non-owning ptr
    // with caller-controlled lifetime, exactly like id/label/unit.
    if (action_buf) {
        JsonObjectConst action = el["action"].as<JsonObjectConst>();
        if (!action.isNull()) {
            const char *kind = action["kind"] | "";
            const char *target = action["target"] | "";
            if (target[0]) {
                copy32(action_buf, target);
                if (!strcmp(kind, "nav")) {
                    out.target_screen = action_buf;
                } else if (!strcmp(kind, "command") || !strcmp(kind, "put")) {
                    // "put" (SignalK PUT to a dotted path) rides the command
                    // slot; button_action_cb routes a dotted, space-free
                    // target through the SignalK PUT queue instead of the
                    // console-command funnel (Race-screen tack dialect).
                    out.command = action_buf;
                    // Optional action.value: JSON-encode it so the PUT sends
                    // the authored payload instead of the fixed `true`
                    // action-trigger convention (sk::putValue takes a JSON
                    // value string; see aphud::put_state's "\"auto\"").
                    if (action_value_buf) {
                        JsonVariantConst av = action["value"];
                        if (av.is<bool>()) {
                            copy32(action_value_buf, av.as<bool>() ? "true" : "false");
                            out.action_value = action_value_buf;
                        } else if (av.is<float>()) {
                            // %g (6 sig. digits) is stable across the float/
                            // double JsonFloat configs; ample for PUT payloads.
                            snprintf(action_value_buf, 32, "%g", av.as<double>());
                            out.action_value = action_value_buf;
                        } else if (av.is<const char *>()) {
                            snprintf(action_value_buf, 32, "\"%s\"", av.as<const char *>());
                            out.action_value = action_value_buf;
                        }
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace midl::render
