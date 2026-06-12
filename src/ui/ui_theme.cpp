#include "ui_theme.h"

namespace ui {

// Default = night theme, "glass cockpit" language (Garmin GPSMAP school):
// cool near-black ground, bordered cells with a subtle vertical gradient for
// depth, semantic side colors. Field order must match struct Palette exactly.
//                bg        panel     panel_bot panel_edge badge     fg
//                fg_dim    accent    warn      alarm     good      port
//                starboard grid
Palette theme = {
    0x0a1018, 0x101b29, 0x0b1320, 0x1f2d3d, 0x16222f, 0xeef4fa, 0x8fa7bd,
    0x4fc3f7, 0xffb84d, 0xff5252, 0x36d399, 0xff5252, 0x36d399, 0x2a3a4c,
};

void use_night() {
    theme = {
        0x0a1018, 0x101b29, 0x0b1320, 0x1f2d3d, 0x16222f, 0xeef4fa, 0x8fa7bd,
        0x4fc3f7, 0xffb84d, 0xff5252, 0x36d399, 0xff5252, 0x36d399, 0x2a3a4c,
    };
}

void use_day() {
    // Sun-readable glass cockpit: bright panels with a faint gradient, dark
    // text, saturated semantic colors. Same chrome structure as night.
    theme = {
        0xeef1f4, 0xffffff, 0xeef2f6, 0xc7d2da, 0xe7edf2, 0x14211d, 0x5a6b78,
        0x0277bd, 0xb56f00, 0xc62828, 0x1b7a4b, 0xc62828, 0x1b7a4b, 0x9fb0bd,
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
    // Glass-cockpit cell: subtle top->bottom vertical gradient for depth,
    // 10px radius, hairline border (accent-tinted when a widget is in an
    // alert/active state). All metrics come from ui::chrome (no inline magic).
    lv_obj_set_style_bg_color(o, c(theme.panel), 0);
    lv_obj_set_style_bg_grad_color(o, c(theme.panel_bot), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, c(accent ? accent : theme.panel_edge), 0);
    lv_obj_set_style_border_opa(o, accent ? LV_OPA_80 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, chrome::panel_border, 0);
    lv_obj_set_style_radius(o, chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(o, chrome::panel_pad, 0);
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
