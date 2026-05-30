#include "ui_theme.h"

namespace ui {

// Default = night theme. The palette avoids an all-blue UI: green-black
// background, blue-cyan navigation accents, amber caution, red/green side
// colors. This keeps cockpit readability while giving each state a job.
Palette theme = {
    /*bg*/ 0x06110f,
    /*panel*/ 0x10201f,
    /*panel_edge*/ 0x34504d,
    /*fg*/ 0xf3f7f2,
    /*fg_dim*/ 0x8fa59d,
    /*accent*/ 0x57c7d8,
    /*warn*/ 0xffb84d,
    /*alarm*/ 0xff4058,
    /*good*/ 0x39d98a,
    /*port*/ 0xff4d6d,
    /*starboard*/ 0x39d98a,
    /*grid*/ 0x52736f,
};

void use_night() {
    theme = {
        0x06110f, 0x10201f, 0x34504d, 0xf3f7f2, 0x8fa59d, 0x57c7d8,
        0xffb84d, 0xff4058, 0x39d98a, 0xff4d6d, 0x39d98a, 0x52736f,
    };
}

void use_day() {
    // Sun-readable: warm white background, dark green-black text, saturated
    // accents. Kept slightly warm to avoid a clinical gray dashboard.
    theme = {
        0xf6f7f2, 0xffffff, 0xc7d2cc, 0x14211d, 0x60746d, 0x006d83,
        0xb56f00, 0xb00020, 0x12804d, 0xb00020, 0x12804d, 0x829891,
    };
}

void style_screen(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, c(theme.bg), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void style_panel(lv_obj_t *o, uint32_t accent) {
    lv_obj_set_style_bg_color(o, c(theme.panel), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, c(accent ? accent : theme.panel_edge), 0);
    lv_obj_set_style_border_opa(o, accent ? LV_OPA_70 : LV_OPA_40, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_pad_all(o, 10, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void style_caption(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(o, c(theme.fg_dim), 0);
}

void style_value(lv_obj_t *o, const lv_font_t *font, uint32_t color) {
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, c(color), 0);
}

lv_obj_t *panel_accent(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_set_size(a, 4, 28);
    lv_obj_set_pos(a, 0, 0);
    lv_obj_set_style_bg_color(a, c(color), 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a, 0, 0);
    lv_obj_set_style_radius(a, 2, 0);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

}  // namespace ui
