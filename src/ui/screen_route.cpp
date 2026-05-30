#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen route page: DTW + BTW + CTS + XTE + VMG + ETA. Reads from the
// fields populated by the courseRhumbline.* SignalK paths. If no route is
// active on the server, shows a friendly placeholder.

namespace ui::route {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_dtw, *lbl_dtw_unit;
static lv_obj_t *lbl_btw_value, *lbl_cts_value;
static lv_obj_t *lbl_vmg_value, *lbl_xte_value;
static lv_obj_t *lbl_eta, *lbl_ttg;
static lv_obj_t *no_route_msg;

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
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
    return c;
}

static lv_obj_t *make_kv(lv_obj_t *parent, const char *cap, const lv_font_t *font, uint32_t color) {
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, cap);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_font(v, font, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return v;
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
    lv_label_set_text(title, "ROUTE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Hero: DTW
    lv_obj_t *hero = make_card(s_root, 8, 40, LCD_W - 16, 160);
    lv_obj_t *cap = lv_label_create(hero);
    lv_label_set_text(cap, "DISTANCE TO WAYPOINT");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_dtw = lv_label_create(hero);
    lv_label_set_text(lbl_dtw, "--.--");
    lv_obj_set_style_text_font(lbl_dtw, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_dtw, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_dtw, LV_ALIGN_CENTER, -30, 10);

    lbl_dtw_unit = lv_label_create(hero);
    lv_label_set_text(lbl_dtw_unit, "nm");
    lv_obj_set_style_text_font(lbl_dtw_unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_dtw_unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_dtw_unit, LV_ALIGN_RIGHT_MID, -20, 10);

    // 4-way grid: BTW, CTS, VMG, XTE
    int col_w = (LCD_W - 24) / 2;
    int row_y = 208;
    int row_h = 80;
    lv_obj_t *c_btw = make_card(s_root, 8, row_y, col_w, row_h);
    lbl_btw_value = make_kv(c_btw, "BTW", &lv_font_montserrat_28, theme.fg);
    lv_obj_t *c_cts = make_card(s_root, 16 + col_w, row_y, col_w, row_h);
    lbl_cts_value = make_kv(c_cts, "CTS", &lv_font_montserrat_28, theme.warn);

    lv_obj_t *c_vmg = make_card(s_root, 8, row_y + row_h + 8, col_w, row_h);
    lbl_vmg_value = make_kv(c_vmg, "VMG", &lv_font_montserrat_28, theme.good);
    lv_obj_t *c_xte = make_card(s_root, 16 + col_w, row_y + row_h + 8, col_w, row_h);
    lbl_xte_value = make_kv(c_xte, "XTE", &lv_font_montserrat_28, theme.fg);

    // ETA / TTG strip
    lv_obj_t *strip = make_card(s_root, 8, row_y + 2 * (row_h + 8), LCD_W - 16, 56);
    lv_obj_set_style_pad_all(strip, 8, 0);
    lv_obj_t *ttg_cap = lv_label_create(strip);
    lv_label_set_text(ttg_cap, "TTG");
    lv_obj_set_style_text_font(ttg_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ttg_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(ttg_cap, LV_ALIGN_LEFT_MID, 0, -8);

    lbl_ttg = lv_label_create(strip);
    lv_label_set_text(lbl_ttg, "--:--");
    lv_obj_set_style_text_font(lbl_ttg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_ttg, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_ttg, LV_ALIGN_LEFT_MID, 0, 10);

    lv_obj_t *eta_cap = lv_label_create(strip);
    lv_label_set_text(eta_cap, "ETA");
    lv_obj_set_style_text_font(eta_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eta_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(eta_cap, LV_ALIGN_RIGHT_MID, 0, -8);

    lbl_eta = lv_label_create(strip);
    lv_label_set_text(lbl_eta, "--:--");
    lv_obj_set_style_text_font(lbl_eta, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_eta, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_eta, LV_ALIGN_RIGHT_MID, 0, 10);

    // "No route" overlay
    no_route_msg = lv_label_create(s_root);
    lv_label_set_text(no_route_msg, "no active route");
    lv_obj_set_style_text_font(no_route_msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(no_route_msg, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(no_route_msg, LV_ALIGN_BOTTOM_MID, 0, -4);

    return s_root;
}

// Dirty-value caches (docs/specs/09).
static char s_last_dtw[16] = {(char)0xFF};
static char s_last_dtw_unit[8] = {(char)0xFF};
static char s_last_btw[16] = {(char)0xFF};
static char s_last_cts[16] = {(char)0xFF};
static char s_last_vmg[16] = {(char)0xFF};
static char s_last_xte[24] = {(char)0xFF};
static char s_last_ttg[16] = {(char)0xFF};
static char s_last_eta[16] = {(char)0xFF};
static int8_t s_last_no_route_hidden = -1;

void refresh() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    bool have_route = !isnan(d.dtw) || !isnan(d.btw) || !isnan(d.cts) || !isnan(d.xte);
    set_hidden_if_changed(no_route_msg, &s_last_no_route_hidden, have_route);

    if (!isnan(d.dtw)) {
        if (d.dtw >= 1852.0) {
            double nm = d.dtw / 1852.0;
            snprintf(buf, sizeof(buf), nm >= 10 ? "%.1f" : "%.2f", nm);
            set_text_if_changed(lbl_dtw, s_last_dtw, sizeof(s_last_dtw), buf);
            set_text_if_changed(lbl_dtw_unit, s_last_dtw_unit, sizeof(s_last_dtw_unit), "nm");
        } else {
            snprintf(buf, sizeof(buf), "%.0f", d.dtw);
            set_text_if_changed(lbl_dtw, s_last_dtw, sizeof(s_last_dtw), buf);
            set_text_if_changed(lbl_dtw_unit, s_last_dtw_unit, sizeof(s_last_dtw_unit), "m");
        }
    } else {
        set_text_if_changed(lbl_dtw, s_last_dtw, sizeof(s_last_dtw), "--.--");
    }

    if (!isnan(d.btw)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.btw));
        set_text_if_changed(lbl_btw_value, s_last_btw, sizeof(s_last_btw), buf);
    } else {
        set_text_if_changed(lbl_btw_value, s_last_btw, sizeof(s_last_btw), "--\xC2\xB0");
    }

    if (!isnan(d.cts)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cts));
        set_text_if_changed(lbl_cts_value, s_last_cts, sizeof(s_last_cts), buf);
    } else {
        set_text_if_changed(lbl_cts_value, s_last_cts, sizeof(s_last_cts), "--\xC2\xB0");
    }

    if (!isnan(d.vmg)) {
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.vmg));
        set_text_if_changed(lbl_vmg_value, s_last_vmg, sizeof(s_last_vmg), buf);
    } else if (!isnan(d.sog) && !isnan(d.cogTrue) && !isnan(d.btw)) {
        double delta = d.cogTrue - d.btw;
        double vmg = d.sog * cos(delta);
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(vmg));
        set_text_if_changed(lbl_vmg_value, s_last_vmg, sizeof(s_last_vmg), buf);
    } else {
        set_text_if_changed(lbl_vmg_value, s_last_vmg, sizeof(s_last_vmg), "-.- kn");
    }

    if (!isnan(d.xte)) {
        const char *side = d.xte > 0 ? "STBD" : (d.xte < 0 ? "PORT" : "");
        if (fabs(d.xte) >= 1852.0)
            snprintf(buf, sizeof(buf), "%.2f nm %s", fabs(d.xte) / 1852.0, side);
        else
            snprintf(buf, sizeof(buf), "%.0f m %s", fabs(d.xte), side);
        set_text_if_changed(lbl_xte_value, s_last_xte, sizeof(s_last_xte), buf);
    } else {
        set_text_if_changed(lbl_xte_value, s_last_xte, sizeof(s_last_xte), "- m");
    }

    double speed = NAN;
    if (!isnan(d.vmg) && d.vmg > 0.05)
        speed = d.vmg;
    else if (!isnan(d.sog) && d.sog > 0.05)
        speed = d.sog;
    if (!isnan(d.dtw) && !isnan(speed)) {
        double secs = d.dtw / speed;
        if (secs < 36000) {
            uint32_t s = (uint32_t)secs;
            snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 3600),
                     (unsigned long)((s / 60) % 60));
        } else {
            snprintf(buf, sizeof(buf), ">10h");
        }
        set_text_if_changed(lbl_ttg, s_last_ttg, sizeof(s_last_ttg), buf);
        time_t now = time(nullptr);
        if (now > 1700000000) {
            now += (time_t)secs;
            struct tm tmv;
            localtime_r(&now, &tmv);
            snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            set_text_if_changed(lbl_eta, s_last_eta, sizeof(s_last_eta), buf);
        } else {
            set_text_if_changed(lbl_eta, s_last_eta, sizeof(s_last_eta), "no clock");
        }
    } else {
        set_text_if_changed(lbl_ttg, s_last_ttg, sizeof(s_last_ttg), "--:--");
        set_text_if_changed(lbl_eta, s_last_eta, sizeof(s_last_eta), "--:--");
    }
}

}  // namespace ui::route
