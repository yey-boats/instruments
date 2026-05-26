#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_screens.h"
#include "net.h"

#include <math.h>
#include "board_pins.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>

// Settings screen - hidden from the swipe cycle, opened by swipe-up from
// any screen. Exposes the most-used knobs: brightness, theme, position
// format, trip reset, demo, MOB clear, and a shortcut to WiFi setup.

namespace ui::settings {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *bright_slider = nullptr;
static lv_obj_t *bright_value = nullptr;
static lv_obj_t *fmt_lbl = nullptr;

static void apply_brightness(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    // Perceptual brightness: human vision is logarithmic, but the LEDC
    // duty cycle is linear, so the slider feels useless above ~50%. Map
    // slider value -> duty via a gamma curve (gamma ~= 2.2) so the
    // displayed change matches what the eye expects.
    double norm = (double)v / 255.0;
    double curved = pow(norm, 2.2);
    int duty = (int)(curved * 255.0 + 0.5);
    // Keep a small floor so 1% slider doesn't fully black out the panel.
    if (v > 0 && duty < 2) duty = 2;
    ledcWrite(0, duty);
    Preferences p;
    p.begin("ui", false);
    p.putUChar("bright", (uint8_t)v);
    p.end();
}

static void on_bright_slider(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int v = lv_slider_get_value(bright_slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(bright_value, buf);
    apply_brightness(v);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                          uint32_t color, lv_event_cb_t cb, void *user) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

static void on_theme_day(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    net::dispatchCommand("theme day");
}
static void on_theme_night(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    net::dispatchCommand("theme night");
}
static void update_fmt_label() {
    if (!fmt_lbl) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "FORMAT  %s", pos_format_name(pos_format()));
    lv_label_set_text(fmt_lbl, buf);
}
static void on_fmt_ddm(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    set_pos_format(PosFormat::DDM); update_fmt_label();
}
static void on_fmt_dd(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    set_pos_format(PosFormat::DD); update_fmt_label();
}
static void on_fmt_dms(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    set_pos_format(PosFormat::DMS); update_fmt_label();
}
static void on_trip_reset(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::trip::reset();
}
static void on_demo_on(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    net::dispatchCommand("demo 4");
}
static void on_demo_off(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    net::dispatchCommand("demo-off");
}
static void on_open_wifi(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::show_by_id("wifi");
}
static void on_close(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::show_by_id("dashboard");
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 8, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Title bar with close
    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 4);

    // Close button moved to the LEFT - the top-right corner is occupied
    // by the global MOB button on lv_layer_top, which would intercept
    // taps meant for "close".
    make_btn(s_root, "close", 12, 6, 88, 36, theme.fg_dim, on_close, nullptr);
    // Move the title to the right so close has room on the left.
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 112, 4);

    // Brightness row
    lv_obj_t *bl_cap = lv_label_create(s_root);
    lv_label_set_text(bl_cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(bl_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(bl_cap, 12, 56);

    bright_value = lv_label_create(s_root);
    lv_label_set_text(bright_value, "200");
    lv_obj_set_style_text_font(bright_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bright_value, lv_color_hex(theme.fg), 0);
    lv_obj_align(bright_value, LV_ALIGN_TOP_RIGHT, -12, 56);

    bright_slider = lv_slider_create(s_root);
    lv_obj_set_size(bright_slider, LCD_W - 32, 16);
    lv_obj_set_pos(bright_slider, 16, 84);
    lv_slider_set_range(bright_slider, 20, 255);
    {
        Preferences p;
        p.begin("ui", true);
        uint8_t v = p.getUChar("bright", 200);
        p.end();
        lv_slider_set_value(bright_slider, v, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)v);
        lv_label_set_text(bright_value, buf);
    }
    lv_obj_add_event_cb(bright_slider, on_bright_slider, LV_EVENT_VALUE_CHANGED, NULL);

    // Theme row
    lv_obj_t *th_cap = lv_label_create(s_root);
    lv_label_set_text(th_cap, "THEME");
    lv_obj_set_style_text_font(th_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(th_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(th_cap, 12, 116);
    make_btn(s_root, "day",   12, 134, 220, 48, theme.warn,    on_theme_day,   nullptr);
    make_btn(s_root, "night", 248, 134, 220, 48, theme.panel_edge, on_theme_night, nullptr);

    // Position format row
    lv_obj_t *fcap = lv_label_create(s_root);
    lv_label_set_text(fcap, "POSITION");
    lv_obj_set_style_text_font(fcap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fcap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(fcap, 12, 196);

    fmt_lbl = lv_label_create(s_root);
    lv_obj_set_style_text_font(fmt_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(fmt_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(fmt_lbl, LV_ALIGN_TOP_RIGHT, -12, 192);
    update_fmt_label();

    make_btn(s_root, "DDM", 12, 220, 145, 48, theme.accent, on_fmt_ddm, nullptr);
    make_btn(s_root, "DD",  165, 220, 145, 48, theme.accent, on_fmt_dd, nullptr);
    make_btn(s_root, "DMS", 318, 220, 150, 48, theme.accent, on_fmt_dms, nullptr);

    // Demo / trip / wifi row
    lv_obj_t *acap = lv_label_create(s_root);
    lv_label_set_text(acap, "ACTIONS");
    lv_obj_set_style_text_font(acap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(acap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(acap, 12, 284);

    make_btn(s_root, "demo on",   12, 304, 220, 48, theme.good, on_demo_on,    nullptr);
    make_btn(s_root, "demo off",  248, 304, 220, 48, theme.fg_dim, on_demo_off, nullptr);
    make_btn(s_root, "trip reset", 12, 360, 220, 48, theme.warn, on_trip_reset, nullptr);
    make_btn(s_root, "wifi setup", 248, 360, 220, 48, theme.accent, on_open_wifi, nullptr);

    // Hint
    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "swipe up from any screen to open  swipe down to close");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    return s_root;
}

void refresh() {
    // No live data - everything is event-driven.
}

}  // namespace ui::settings
