#include "widget_registry.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "font_resolver.h"
#include "ui_theme.h"
#include "widget_data_resolver.h"
#include "metric_value.h"     // ui::layouts::metric_value / metric_unit_fraction
#include "layout_renderer.h"  // ui::layout_render::path_to_source
#include "net.h"

using ui::layouts::MetricSource;

namespace widget_registry {

namespace {

const lv_font_t *font_for(uint16_t want, uint16_t fallback) {
    uint16_t size = want > 0 ? want : fallback;
    size = font_resolver::resolve_default(size);
    // Map to LVGL pointer via the Arduino-only TU.
    switch (size) {
    case 14:
        return &lv_font_montserrat_14;
    case 20:
        return &lv_font_montserrat_20;
    case 28:
        return &lv_font_montserrat_28;
    case 48:
        return &lv_font_montserrat_48;
    default:
        return &lv_font_montserrat_14;
    }
}

void make_panel(lv_obj_t *root, int16_t x, int16_t y, int16_t w, int16_t h) {
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_style_bg_color(root, lv_color_hex(ui::theme.panel), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(root, lv_color_hex(ui::theme.panel_edge), 0);
    lv_obj_set_style_border_width(root, 1, 0);
    lv_obj_set_style_radius(root, 6, 0);
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void format_numeric(const Widget &w, double v, char *out, size_t cap) {
    if (isnan(v)) {
        snprintf(out, cap, "--");
        return;
    }
    int p = w.precision > 6 ? 6 : (int)w.precision;
    if (w.unit[0]) {
        snprintf(out, cap, "%.*f %s", p, v, w.unit);
    } else {
        snprintf(out, cap, "%.*f", p, v);
    }
}

// Resolve the widget's path to a display-unit scalar. Known SignalK paths go
// through the shared resolver (m/s->kn, rad->deg, K->C, ...); alias or dynamic
// paths (boat.*, raw deltas) fall back to the raw boat::View field. `src` is
// cached on the Widget at create() so this is a single branch on the hot path.
double display_scalar(const Widget &w, const boat::View &d) {
    if (w.src != MetricSource::None) return ui::layouts::metric_value(w.src, d);
    return widget_data::resolve_numeric(w.path, d);
}

}  // namespace

Widget *create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
               const manager_config::WidgetDef &def, const manager_config::WidgetStyle &defaults) {
    Widget *wd = (Widget *)heap_caps_calloc(1, sizeof(Widget), MALLOC_CAP_INTERNAL);
    if (!wd) {
        net::logf("[wreg] alloc fail for %s", def.id);
        return nullptr;
    }
    wd->type = def.type;
    strncpy(wd->id, def.id, sizeof(wd->id) - 1);
    strncpy(wd->path, def.path, sizeof(wd->path) - 1);
    strncpy(wd->unit, def.unit, sizeof(wd->unit) - 1);
    wd->precision = def.precision;
    wd->min = def.min;
    wd->max = def.max;
    wd->src = ui::layout_render::path_to_source(def.path);

    wd->root = lv_obj_create(parent);
    make_panel(wd->root, x, y, w, h);

    // Title caption (small, fg_dim, top-left)
    wd->title_lbl = lv_label_create(wd->root);
    lv_label_set_text(wd->title_lbl, def.title[0] ? def.title : def.id);
    lv_obj_set_style_text_font(wd->title_lbl,
                               font_for(def.style.label_font_size,
                                        defaults.label_font_size ? defaults.label_font_size : 14),
                               0);
    lv_obj_set_style_text_color(wd->title_lbl, lv_color_hex(ui::theme.fg_dim), 0);
    lv_obj_align(wd->title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    switch (def.type) {
    case manager_config::WidgetType::Numeric:
    case manager_config::WidgetType::Text: {
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(
            wd->value_lbl,
            font_for(def.style.value_font_size,
                     defaults.value_font_size ? defaults.value_font_size : 28),
            0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 6);
        break;
    }
    case manager_config::WidgetType::Bar: {
        wd->bar = lv_bar_create(wd->root);
        lv_obj_set_size(wd->bar, w - 16, 16);
        lv_obj_align(wd->bar, LV_ALIGN_BOTTOM_MID, 0, -4);
        double lo = isnan(def.min) ? 0.0 : def.min;
        double hi = isnan(def.max) ? 100.0 : def.max;
        lv_bar_set_range(wd->bar, (int32_t)lo, (int32_t)hi);
        lv_obj_set_style_bg_color(wd->bar, lv_color_hex(ui::theme.panel_edge), LV_PART_MAIN);
        lv_obj_set_style_bg_color(wd->bar, lv_color_hex(ui::theme.accent), LV_PART_INDICATOR);
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(wd->value_lbl, font_for(def.style.value_font_size, 20), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, -4);
        break;
    }
    case manager_config::WidgetType::Gauge: {
        // 270deg arc (bottom-open) + centred numeric. Range = [min,max] when the
        // manager supplied bounds, else 0..100 (percent-style). The arc fill is
        // driven from the display value in update().
        int side = (w < h ? w : h) - 16;
        if (side < 40) side = 40;
        wd->arc = lv_arc_create(wd->root);
        lv_obj_set_size(wd->arc, side, side);
        lv_obj_align(wd->arc, LV_ALIGN_CENTER, 0, 6);
        lv_arc_set_bg_angles(wd->arc, 135, 45);  // 270deg sweep, bottom open
        double lo = isnan(def.min) ? 0.0 : def.min;
        double hi = isnan(def.max) ? 100.0 : def.max;
        if (hi <= lo) hi = lo + 1.0;
        lv_arc_set_range(wd->arc, (int32_t)lround(lo), (int32_t)lround(hi));
        lv_arc_set_value(wd->arc, (int32_t)lround(lo));
        lv_obj_remove_style(wd->arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(wd->arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(wd->arc, lv_color_hex(ui::theme.grid), LV_PART_MAIN);
        lv_obj_set_style_arc_width(wd->arc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_color(wd->arc, lv_color_hex(ui::theme.accent), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(wd->arc, 8, LV_PART_INDICATOR);
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(wd->value_lbl, font_for(def.style.value_font_size, 28), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 6);
        break;
    }
    case manager_config::WidgetType::Compass:
    case manager_config::WidgetType::WindRose: {
        // Circular bezel ring + centred heading/wind angle + a secondary line
        // (cardinal letter for compass, port/stbd for wind). A full rotating
        // marker ring is the MIDL renderer's job; this lightweight tile shows
        // the numeric bearing inside a bezel.
        int side = (w < h ? w : h) - 16;
        if (side < 40) side = 40;
        wd->arc = lv_obj_create(wd->root);
        lv_obj_set_size(wd->arc, side, side);
        lv_obj_align(wd->arc, LV_ALIGN_CENTER, 0, 6);
        lv_obj_set_style_radius(wd->arc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(wd->arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(wd->arc, lv_color_hex(ui::theme.grid), 0);
        lv_obj_set_style_border_width(wd->arc, 3, 0);
        lv_obj_remove_flag(wd->arc, LV_OBJ_FLAG_SCROLLABLE);
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(wd->value_lbl, font_for(def.style.value_font_size, 28), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 0);
        wd->sub_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->sub_lbl, "");
        lv_obj_set_style_text_font(wd->sub_lbl, font_for(0, 14), 0);
        lv_obj_set_style_text_color(wd->sub_lbl, lv_color_hex(ui::theme.fg_dim), 0);
        lv_obj_align(wd->sub_lbl, LV_ALIGN_CENTER, 0, 22);
        break;
    }
    case manager_config::WidgetType::Trend: {
        // Rolling sparkline (LINE chart, no axes). Values are pushed normalised
        // to 0..100 via the shared per-source fraction so any metric fits.
        wd->chart = lv_chart_create(wd->root);
        lv_obj_set_size(wd->chart, w - 16, h - 30);
        lv_obj_align(wd->chart, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_chart_set_type(wd->chart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(wd->chart, 30);
        lv_chart_set_div_line_count(wd->chart, 0, 0);
        lv_chart_set_range(wd->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_obj_set_style_width(wd->chart, 0, LV_PART_INDICATOR);  // no point dots
        lv_obj_set_style_height(wd->chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(wd->chart, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wd->chart, 0, 0);
        wd->series =
            lv_chart_add_series(wd->chart, lv_color_hex(ui::theme.accent), LV_CHART_AXIS_PRIMARY_Y);
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(wd->value_lbl, font_for(0, 14), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
        break;
    }
    case manager_config::WidgetType::Button: {
        // The Spec-19 WidgetDef carries no action target, so this is a styled,
        // non-interactive label box (a real command/nav button needs the MIDL
        // renderer's action plumbing). Big centred title in an accent frame.
        lv_obj_set_style_border_color(wd->root, lv_color_hex(ui::theme.accent), 0);
        lv_obj_set_style_border_width(wd->root, 2, 0);
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, def.title[0] ? def.title : def.id);
        lv_obj_set_style_text_font(wd->value_lbl, font_for(def.style.value_font_size, 28), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.accent), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(wd->title_lbl, LV_OBJ_FLAG_HIDDEN);  // no redundant caption
        break;
    }
    case manager_config::WidgetType::Autopilot: {
        // Autopilot state pill (AUTO / STBY / ...) + target/heading line.
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, "--");
        lv_obj_set_style_text_font(wd->value_lbl, font_for(def.style.value_font_size, 28), 0);
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg), 0);
        lv_obj_set_style_bg_color(wd->value_lbl, lv_color_hex(ui::theme.panel_edge), 0);
        lv_obj_set_style_bg_opa(wd->value_lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(wd->value_lbl, 8, 0);
        lv_obj_set_style_pad_hor(wd->value_lbl, 12, 0);
        lv_obj_set_style_pad_ver(wd->value_lbl, 6, 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 0);
        break;
    }
    default: {
        wd->value_lbl = lv_label_create(wd->root);
        lv_label_set_text(wd->value_lbl, manager_config::widget_type_to_string(def.type));
        lv_obj_set_style_text_color(wd->value_lbl, lv_color_hex(ui::theme.fg_dim), 0);
        lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 6);
        net::logf("[wreg] %s (type=%s) - unknown widget", def.id,
                  manager_config::widget_type_to_string(def.type));
        break;
    }
    }
    return wd;
}

void destroy(Widget *w) {
    if (!w) return;
    heap_caps_free(w);
}

namespace {

// 8-point cardinal abbreviation for a heading in degrees [0,360).
const char *cardinal8(double deg) {
    static const char *C[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = ((int)lround(deg / 45.0)) & 7;
    return C[idx];
}

}  // namespace

void update(Widget &w, const boat::View &data) {
    char buf[64];
    switch (w.type) {
    case manager_config::WidgetType::Numeric: {
        if (!w.value_lbl) return;
        double v = display_scalar(w, data);
        format_numeric(w, v, buf, sizeof(buf));
        lv_label_set_text(w.value_lbl, buf);
        break;
    }
    case manager_config::WidgetType::Text: {
        if (!w.value_lbl) return;
        char txt[32];
        if (widget_data::resolve_string(w.path, data, txt, sizeof(txt))) {
            lv_label_set_text(w.value_lbl, txt);
        } else {
            lv_label_set_text(w.value_lbl, "--");
        }
        break;
    }
    case manager_config::WidgetType::Bar: {
        if (!w.bar) return;
        double v = display_scalar(w, data);
        if (isnan(v)) {
            lv_label_set_text(w.value_lbl, "--");
        } else {
            lv_bar_set_value(w.bar, (int32_t)lround(v), LV_ANIM_OFF);
            format_numeric(w, v, buf, sizeof(buf));
            lv_label_set_text(w.value_lbl, buf);
        }
        break;
    }
    case manager_config::WidgetType::Gauge: {
        if (!w.arc || !w.value_lbl) return;
        double v = display_scalar(w, data);
        if (isnan(v)) {
            lv_label_set_text(w.value_lbl, "--");
        } else {
            lv_arc_set_value(w.arc, (int32_t)lround(v));  // arc clamps to its range
            format_numeric(w, v, buf, sizeof(buf));
            lv_label_set_text(w.value_lbl, buf);
        }
        break;
    }
    case manager_config::WidgetType::Compass: {
        if (!w.value_lbl) return;
        double deg = display_scalar(w, data);
        if (isnan(deg)) {
            lv_label_set_text(w.value_lbl, "--");
            if (w.sub_lbl) lv_label_set_text(w.sub_lbl, "");
        } else {
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", deg);
            lv_label_set_text(w.value_lbl, buf);
            if (w.sub_lbl) lv_label_set_text(w.sub_lbl, cardinal8(deg));
        }
        break;
    }
    case manager_config::WidgetType::WindRose: {
        if (!w.value_lbl) return;
        double deg = display_scalar(w, data);  // [0,360)
        if (isnan(deg)) {
            lv_label_set_text(w.value_lbl, "--");
            if (w.sub_lbl) lv_label_set_text(w.sub_lbl, "");
        } else {
            double rel = deg > 180.0 ? deg - 360.0 : deg;  // -180..180
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", rel < 0 ? -rel : rel);
            lv_label_set_text(w.value_lbl, buf);
            if (w.sub_lbl) lv_label_set_text(w.sub_lbl, rel < 0 ? "Port" : "Stbd");
        }
        break;
    }
    case manager_config::WidgetType::Trend: {
        if (!w.chart || !w.series) return;
        double v = display_scalar(w, data);
        double frac = ui::layouts::metric_unit_fraction(w.src, v);
        if (!isnan(frac)) {
            lv_chart_set_next_value(w.chart, w.series, (int32_t)lround(frac * 100.0));
            lv_chart_refresh(w.chart);
        }
        if (w.value_lbl) {
            if (isnan(v)) {
                lv_label_set_text(w.value_lbl, "--");
            } else {
                format_numeric(w, v, buf, sizeof(buf));
                lv_label_set_text(w.value_lbl, buf);
            }
        }
        break;
    }
    case manager_config::WidgetType::Autopilot: {
        if (!w.value_lbl) return;
        char st[16];
        if (widget_data::resolve_string("steering.autopilot.state", data, st, sizeof(st))) {
            // Uppercase for a compact pill: "auto" -> "AUTO".
            for (char *p = st; *p; ++p)
                *p = (char)toupper((unsigned char)*p);
            lv_label_set_text(w.value_lbl, st);
            bool engaged = strcmp(st, "STANDBY") != 0 && strcmp(st, "OFF") != 0;
            lv_obj_set_style_bg_color(
                w.value_lbl, lv_color_hex(engaged ? ui::theme.good : ui::theme.panel_edge), 0);
        } else {
            lv_label_set_text(w.value_lbl, "--");
            lv_obj_set_style_bg_color(w.value_lbl, lv_color_hex(ui::theme.panel_edge), 0);
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace widget_registry
