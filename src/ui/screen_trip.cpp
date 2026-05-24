#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "net.h"
#include "board_pins.h"

#include <Preferences.h>
#include <math.h>
#include <stdio.h>

// Trip / log screen: locally-integrated odometer + stats. Persists trip
// state to NVS so it survives reboots. SignalK only provides instantaneous
// SOG; we accumulate distance and stats ourselves.

namespace ui::trip {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_dist, *lbl_dist_unit;
static lv_obj_t *lbl_time, *lbl_avg, *lbl_max;
static lv_obj_t *lbl_sog_now;
static lv_obj_t *lbl_tip;

static double s_dist_m = 0;      // accumulated distance, meters
static uint32_t s_underway_s = 0;  // accumulated seconds with sog > threshold
static double s_max_sog = 0;     // m/s
static uint32_t s_started_at_ms = 0;
static uint32_t s_last_sample_ms = 0;
static const double UNDERWAY_THRESHOLD = 0.5;  // m/s ~= 1 kn

static void load_from_nvs() {
    Preferences p;
    p.begin("trip", true);
    s_dist_m = p.getDouble("dist", 0);
    s_underway_s = p.getUInt("under", 0);
    s_max_sog = p.getDouble("maxsog", 0);
    s_started_at_ms = p.getUInt("start", 0);
    p.end();
    if (s_started_at_ms == 0) s_started_at_ms = millis();
}

static void save_to_nvs() {
    Preferences p;
    p.begin("trip", false);
    p.putDouble("dist", s_dist_m);
    p.putUInt("under", s_underway_s);
    p.putDouble("maxsog", s_max_sog);
    p.putUInt("start", s_started_at_ms);
    p.end();
}

void reset() {
    s_dist_m = 0;
    s_underway_s = 0;
    s_max_sog = 0;
    s_started_at_ms = millis();
    s_last_sample_ms = 0;
    save_to_nvs();
    net::logf("[trip] reset");
}

static lv_obj_t *make_stat(lv_obj_t *parent, const char *cap, int x, int y, int w, int h,
                            lv_obj_t **value_out, const lv_font_t *font, uint32_t color) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *ttl = lv_label_create(c);
    lv_label_set_text(ttl, cap);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(c);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_font(val, font, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(color), 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    *value_out = val;
    return c;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "TRIP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Hero distance card spans top half
    lv_obj_t *hero = lv_obj_create(s_root);
    lv_obj_set_size(hero, LCD_W - 16, 200);
    lv_obj_set_pos(hero, 8, 40);
    lv_obj_set_style_bg_color(hero, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_pad_all(hero, 8, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *cap = lv_label_create(hero);
    lv_label_set_text(cap, "DISTANCE");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_dist = lv_label_create(hero);
    lv_label_set_text(lbl_dist, "0.00");
    lv_obj_set_style_text_font(lbl_dist, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_dist, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_dist, LV_ALIGN_CENTER, -30, 10);

    lbl_dist_unit = lv_label_create(hero);
    lv_label_set_text(lbl_dist_unit, "nm");
    lv_obj_set_style_text_font(lbl_dist_unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_dist_unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_dist_unit, LV_ALIGN_RIGHT_MID, -20, 10);

    // Three stats side by side
    int row_y = 248;
    int col_w = (LCD_W - 32) / 3;
    make_stat(s_root, "TIME UNDERWAY", 8, row_y, col_w, 100, &lbl_time,
              &lv_font_montserrat_28, theme.fg);
    make_stat(s_root, "AVG SPEED", 8 + col_w + 8, row_y, col_w, 100, &lbl_avg,
              &lv_font_montserrat_28, theme.fg);
    make_stat(s_root, "MAX SPEED", 8 + (col_w + 8) * 2, row_y, col_w - 8, 100, &lbl_max,
              &lv_font_montserrat_28, theme.good);

    // Live SOG strip
    lv_obj_t *strip = lv_obj_create(s_root);
    lv_obj_set_size(strip, LCD_W - 16, 60);
    lv_obj_set_pos(strip, 8, 356);
    lv_obj_set_style_bg_color(strip, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(strip, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_style_radius(strip, 8, 0);
    lv_obj_set_style_pad_all(strip, 8, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *now_cap = lv_label_create(strip);
    lv_label_set_text(now_cap, "NOW");
    lv_obj_set_style_text_font(now_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(now_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(now_cap, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_sog_now = lv_label_create(strip);
    lv_label_set_text(lbl_sog_now, "--.- kn");
    lv_obj_set_style_text_font(lbl_sog_now, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_sog_now, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_sog_now, LV_ALIGN_CENTER, 0, 0);

    lbl_tip = lv_label_create(s_root);
    lv_label_set_text(lbl_tip, "console: trip-reset");
    lv_obj_set_style_text_font(lbl_tip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_tip, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_tip, LV_ALIGN_BOTTOM_MID, 0, -4);

    load_from_nvs();
    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    // Integrate SOG over real elapsed time (sample at most 1 Hz).
    uint32_t now = millis();
    if (s_last_sample_ms == 0) s_last_sample_ms = now;
    if (!isnan(d.sog) && now - s_last_sample_ms >= 1000) {
        double dt_s = (now - s_last_sample_ms) / 1000.0;
        s_last_sample_ms = now;
        if (d.sog > UNDERWAY_THRESHOLD) {
            s_dist_m += d.sog * dt_s;
            s_underway_s += (uint32_t)dt_s;
        }
        if (d.sog > s_max_sog) s_max_sog = d.sog;
        // Save every ~30 s of underway time so a power-off doesn't lose much
        if ((s_underway_s % 30) == 0) save_to_nvs();
    }

    double nm = s_dist_m / 1852.0;
    if (nm >= 10) snprintf(buf, sizeof(buf), "%.1f", nm);
    else snprintf(buf, sizeof(buf), "%.2f", nm);
    lv_label_set_text(lbl_dist, buf);

    uint32_t hh = s_underway_s / 3600;
    uint32_t mm = (s_underway_s / 60) % 60;
    uint32_t ss = s_underway_s % 60;
    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm,
             (unsigned long)ss);
    lv_label_set_text(lbl_time, buf);

    if (s_underway_s > 5) {
        double avg_kn = mps_to_kn(s_dist_m / s_underway_s);
        snprintf(buf, sizeof(buf), "%.1f kn", avg_kn);
    } else {
        snprintf(buf, sizeof(buf), "-.-- kn");
    }
    lv_label_set_text(lbl_avg, buf);

    snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(s_max_sog));
    lv_label_set_text(lbl_max, buf);

    if (!isnan(d.sog))
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.sog));
    else
        snprintf(buf, sizeof(buf), "-.- kn");
    lv_label_set_text(lbl_sog_now, buf);
}

}  // namespace ui::trip
