#include "ui_theme.h"

#include <string.h>

namespace ui {

// Default = night theme (palettes::night). The colour tables themselves live
// in ui_palette.h so the native contrast test can check them.
Palette theme = palettes::night;

// Canonical name of the active palette; kept in lockstep by the use_*()
// setters so console/web/state endpoints can report the live theme.
static const char *s_theme_id = "night";

static void apply(const Palette &p, const char *id) {
    theme = p;
    s_theme_id = id;
}

void use_night() {
    apply(palettes::night, "night");
}
void use_day() {
    apply(palettes::day, "day");
}
void use_high_contrast() {
    apply(palettes::high_contrast, "high-contrast");
}
void use_red_night() {
    apply(palettes::red_night, "red-night");
}
void use_classic() {
    apply(palettes::classic, "classic");
}
void use_sun() {
    apply(palettes::sun, "sun");
}

bool theme_known(const char *name) {
    if (!name || !*name) return false;
    for (const auto &t : palettes::all)
        if (strcmp(t.name, name) == 0) return true;
    return false;
}

bool use_theme(const char *name) {
    if (!name) return false;
    for (const auto &t : palettes::all) {
        if (strcmp(t.name, name) == 0) {
            apply(*t.palette, t.name);
            return true;
        }
    }
    return false;
}

const char *theme_id() {
    return s_theme_id;
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
    lv_obj_set_style_text_font(o, &font_b612_14, 0);
    lv_obj_set_style_text_color(o, c(theme.fg_dim), 0);
}

void style_hud_backing(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, c(theme.panel), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_set_style_radius(o, 4, 0);
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
