#include "widget_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "font_resolver.h"
#include "ui_theme.h"
#include "widget_data_resolver.h"
#include "net.h"

namespace widget_registry {

namespace {

const lv_font_t *font_for(uint16_t want, uint16_t fallback) {
    uint16_t size = want > 0 ? want : fallback;
    size = font_resolver::resolve_default(size);
    // Map to LVGL pointer via the Arduino-only TU.
    switch (size) {
        case 14: return &lv_font_montserrat_14;
        case 20: return &lv_font_montserrat_20;
        case 28: return &lv_font_montserrat_28;
        case 48: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_14;
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
    if (isnan(v)) { snprintf(out, cap, "--"); return; }
    int p = w.precision > 6 ? 6 : (int)w.precision;
    if (w.unit[0]) {
        snprintf(out, cap, "%.*f %s", p, v, w.unit);
    } else {
        snprintf(out, cap, "%.*f", p, v);
    }
}

}  // namespace

Widget *create(lv_obj_t *parent,
               int16_t x, int16_t y, int16_t w, int16_t h,
               const manager_config::WidgetDef &def,
               const manager_config::WidgetStyle &defaults) {
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

    wd->root = lv_obj_create(parent);
    make_panel(wd->root, x, y, w, h);

    // Title caption (small, fg_dim, top-left)
    wd->title_lbl = lv_label_create(wd->root);
    lv_label_set_text(wd->title_lbl, def.title[0] ? def.title : def.id);
    lv_obj_set_style_text_font(wd->title_lbl,
        font_for(def.style.label_font_size, defaults.label_font_size ?
                 defaults.label_font_size : 14), 0);
    lv_obj_set_style_text_color(wd->title_lbl, lv_color_hex(ui::theme.fg_dim), 0);
    lv_obj_align(wd->title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    switch (def.type) {
        case manager_config::WidgetType::Numeric:
        case manager_config::WidgetType::Text: {
            wd->value_lbl = lv_label_create(wd->root);
            lv_label_set_text(wd->value_lbl, "--");
            lv_obj_set_style_text_font(wd->value_lbl,
                font_for(def.style.value_font_size,
                         defaults.value_font_size ?
                         defaults.value_font_size : 28), 0);
            lv_obj_set_style_text_color(wd->value_lbl,
                lv_color_hex(ui::theme.fg), 0);
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
            lv_obj_set_style_text_font(wd->value_lbl,
                font_for(def.style.value_font_size, 20), 0);
            lv_obj_set_style_text_color(wd->value_lbl,
                lv_color_hex(ui::theme.fg), 0);
            lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, -4);
            break;
        }
        // Stubs - placeholder label so callers don't crash; D4 follow-up
        // will fill in gauge/compass/trend/button/autopilot.
        case manager_config::WidgetType::Gauge:
        case manager_config::WidgetType::Compass:
        case manager_config::WidgetType::WindRose:
        case manager_config::WidgetType::Trend:
        case manager_config::WidgetType::Button:
        case manager_config::WidgetType::Autopilot:
        default: {
            wd->value_lbl = lv_label_create(wd->root);
            lv_label_set_text(wd->value_lbl,
                              manager_config::widget_type_to_string(def.type));
            lv_obj_set_style_text_color(wd->value_lbl,
                lv_color_hex(ui::theme.fg_dim), 0);
            lv_obj_align(wd->value_lbl, LV_ALIGN_CENTER, 0, 6);
            net::logf("[wreg] %s (type=%s) - stub widget",
                      def.id, manager_config::widget_type_to_string(def.type));
            break;
        }
    }
    return wd;
}

void update(Widget &w, const sk::Data &data) {
    char buf[64];
    switch (w.type) {
        case manager_config::WidgetType::Numeric: {
            if (!w.value_lbl) return;
            double v = widget_data::resolve_numeric(w.path, data);
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
            double v = widget_data::resolve_numeric(w.path, data);
            if (isnan(v)) {
                lv_label_set_text(w.value_lbl, "--");
            } else {
                lv_bar_set_value(w.bar, (int32_t)v, LV_ANIM_OFF);
                format_numeric(w, v, buf, sizeof(buf));
                lv_label_set_text(w.value_lbl, buf);
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace widget_registry
