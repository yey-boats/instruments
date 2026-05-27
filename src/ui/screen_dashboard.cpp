#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "signalk.h"
#include "net.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

namespace ui::dashboard {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_aws, *lbl_awa;
static lv_obj_t *needle;
static lv_obj_t *lbl_sog, *lbl_cog, *lbl_hdg, *lbl_pos;
static lv_obj_t *lbl_depth, *lbl_temp;
static lv_obj_t *lbl_batt, *lbl_status, *lbl_ip, *lbl_rssi;

static lv_obj_t *make_quadrant(lv_obj_t *parent, int qx, int qy, const char *header) {
    lv_obj_t *q = lv_obj_create(parent);
    lv_obj_set_size(q, 232, 232);
    lv_obj_set_pos(q, qx * 240 + 4, qy * 240 + 4);
    uint32_t accent = strcmp(header, "WIND") == 0   ? theme.warn
                      : strcmp(header, "NAV") == 0  ? theme.accent
                      : strcmp(header, "DEPTH") == 0 ? theme.good
                                                     : theme.grid;
    style_panel(q, accent);
    panel_accent(q, accent);

    lv_obj_t *h = lv_label_create(q);
    lv_label_set_text(h, header);
    style_caption(h);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 10, 0);
    return q;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    style_screen(s_root);

    // Wind (top-left)
    lv_obj_t *q1 = make_quadrant(s_root, 0, 0, "WIND");
    lv_obj_t *ring = lv_obj_create(q1);
    lv_obj_set_size(ring, 140, 140);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(theme.grid), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 24);
    needle = lv_obj_create(q1);
    lv_obj_set_size(needle, 4, 64);
    lv_obj_set_style_bg_color(needle, lv_color_hex(theme.port), 0);
    lv_obj_set_style_border_width(needle, 0, 0);
    lv_obj_set_style_radius(needle, 2, 0);
    lv_obj_align(needle, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_transform_pivot_x(needle, 2, 0);
    lv_obj_set_style_transform_pivot_y(needle, 60, 0);

    lbl_aws = lv_label_create(q1);
    lv_label_set_text(lbl_aws, "-- kn");
    style_value(lbl_aws, &lv_font_montserrat_28, theme.fg);
    lv_obj_align(lbl_aws, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lbl_awa = lv_label_create(q1);
    lv_label_set_text(lbl_awa, "---\xC2\xB0");
    style_value(lbl_awa, &lv_font_montserrat_28, theme.warn);
    lv_obj_align(lbl_awa, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // Nav (top-right)
    lv_obj_t *q2 = make_quadrant(s_root, 1, 0, "NAV");
    lbl_sog = lv_label_create(q2);
    lv_label_set_text(lbl_sog, "--.-");
    style_value(lbl_sog, &lv_font_montserrat_48, theme.accent);
    lv_obj_align(lbl_sog, LV_ALIGN_TOP_MID, -16, 18);

    lv_obj_t *sog_unit = lv_label_create(q2);
    lv_label_set_text(sog_unit, "kn");
    style_value(sog_unit, &lv_font_montserrat_20, theme.fg_dim);
    lv_obj_align(sog_unit, LV_ALIGN_TOP_MID, 60, 38);

    lbl_cog = lv_label_create(q2);
    lv_label_set_text(lbl_cog, "COG ---\xC2\xB0");
    style_value(lbl_cog, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_cog, LV_ALIGN_TOP_LEFT, 0, 92);

    lbl_hdg = lv_label_create(q2);
    lv_label_set_text(lbl_hdg, "HDG ---\xC2\xB0");
    style_value(lbl_hdg, &lv_font_montserrat_14, theme.accent);
    lv_obj_align(lbl_hdg, LV_ALIGN_TOP_LEFT, 0, 122);

    lbl_pos = lv_label_create(q2);
    lv_label_set_text(lbl_pos, "---\xC2\xB0--.---'N\n---\xC2\xB0--.---'E");
    style_value(lbl_pos, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_pos, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Depth (bottom-left)
    lv_obj_t *q3 = make_quadrant(s_root, 0, 1, "DEPTH");
    lbl_depth = lv_label_create(q3);
    lv_label_set_text(lbl_depth, "--.-");
    style_value(lbl_depth, &lv_font_montserrat_48, theme.good);
    lv_obj_align(lbl_depth, LV_ALIGN_CENTER, -10, -10);
    lv_obj_t *dunit = lv_label_create(q3);
    lv_label_set_text(dunit, "m");
    style_value(dunit, &lv_font_montserrat_20, theme.fg_dim);
    lv_obj_align(dunit, LV_ALIGN_CENTER, 50, -10);
    lbl_temp = lv_label_create(q3);
    lv_label_set_text(lbl_temp, "water --.- \xC2\xB0""C");
    style_value(lbl_temp, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_temp, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Status (bottom-right)
    lv_obj_t *q4 = make_quadrant(s_root, 1, 1, "SYSTEM");
    lbl_batt = lv_label_create(q4);
    lv_label_set_text(lbl_batt, "--.- V");
    style_value(lbl_batt, &lv_font_montserrat_28, theme.fg);
    lv_obj_align(lbl_batt, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *batt_caption = lv_label_create(q4);
    lv_label_set_text(batt_caption, "BATTERY");
    style_caption(batt_caption);
    lv_obj_align(batt_caption, LV_ALIGN_TOP_MID, 0, 64);

    lbl_status = lv_label_create(q4);
    lv_label_set_text(lbl_status, "sk: -");
    style_value(lbl_status, &lv_font_montserrat_14, theme.fg);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, -36);

    lbl_ip = lv_label_create(q4);
    lv_label_set_text(lbl_ip, "ip ---.---.---.---");
    style_value(lbl_ip, &lv_font_montserrat_14, theme.fg);
    lv_obj_align(lbl_ip, LV_ALIGN_BOTTOM_LEFT, 0, -18);

    lbl_rssi = lv_label_create(q4);
    lv_label_set_text(lbl_rssi, "rssi --- dBm");
    style_value(lbl_rssi, &lv_font_montserrat_14, theme.fg);
    lv_obj_align(lbl_rssi, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return s_root;
}

// Dirty-value caches (docs/specs/09).
static char s_last_aws[16] = {(char)0xFF};
static char s_last_awa[16] = {(char)0xFF};
static char s_last_sog[16] = {(char)0xFF};
static char s_last_cog[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_pos[64] = {(char)0xFF};
static char s_last_depth[16] = {(char)0xFF};
static char s_last_temp[24] = {(char)0xFF};
static char s_last_batt[16] = {(char)0xFF};
static char s_last_status[24] = {(char)0xFF};
static char s_last_ip[24] = {(char)0xFF};
static char s_last_rssi[40] = {(char)0xFF};
static int16_t s_last_needle_rot = INT16_MIN;

void refresh() {
    sk::Data d_snap; sk::copyData(d_snap); const sk::Data &d = d_snap;
    char buf[64];
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.aws));
        set_text_if_changed(lbl_aws, s_last_aws, sizeof(s_last_aws), buf);
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", deg);
        set_text_if_changed(lbl_awa, s_last_awa, sizeof(s_last_awa), buf);
        set_rot_if_changed(needle, &s_last_needle_rot, (int16_t)(deg * 10));
    } else {
        // No-data state: stop animating. The earlier (millis()/4)%3600
        // sweep was a steady source of invalidations across the whole
        // dashboard needle for no information value. Show "no data"
        // instead.
        set_text_if_changed(lbl_awa, s_last_awa, sizeof(s_last_awa), "no data");
    }
    if (!isnan(d.sog)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.sog));
        set_text_if_changed(lbl_sog, s_last_sog, sizeof(s_last_sog), buf);
    }
    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "COG %.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        set_text_if_changed(lbl_cog, s_last_cog, sizeof(s_last_cog), buf);
    }
    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "HDG %.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        set_text_if_changed(lbl_hdg, s_last_hdg, sizeof(s_last_hdg), buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        format_position(d.lat, d.lon, pos_format(), buf, sizeof(buf));
        set_text_if_changed(lbl_pos, s_last_pos, sizeof(s_last_pos), buf);
    }
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        set_text_if_changed(lbl_depth, s_last_depth, sizeof(s_last_depth), buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "water %.1f \xC2\xB0""C", k_to_c(d.waterTemp));
        set_text_if_changed(lbl_temp, s_last_temp, sizeof(s_last_temp), buf);
    }
    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "%.1f V", d.battVoltage);
        set_text_if_changed(lbl_batt, s_last_batt, sizeof(s_last_batt), buf);
    } else {
        snprintf(buf, sizeof(buf), "%lu kB", (unsigned long)(ESP.getFreeHeap() / 1024));
        set_text_if_changed(lbl_batt, s_last_batt, sizeof(s_last_batt), buf);
    }
    snprintf(buf, sizeof(buf), "sk: %s", sk::connectionStatus().c_str());
    set_text_if_changed(lbl_status, s_last_status, sizeof(s_last_status), buf);
    snprintf(buf, sizeof(buf), "ip %s", net::ipString().c_str());
    set_text_if_changed(lbl_ip, s_last_ip, sizeof(s_last_ip), buf);

    int r = net::rssi();
    uint32_t up = millis() / 1000;
    if (r != 0) {
        snprintf(buf, sizeof(buf), "rssi %d dBm  up %lu:%02lu", r, (unsigned long)(up / 60),
                 (unsigned long)(up % 60));
    } else {
        snprintf(buf, sizeof(buf), "ap mode  up %02lu:%02lu:%02lu", (unsigned long)(up / 3600),
                 (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    }
    set_text_if_changed(lbl_rssi, s_last_rssi, sizeof(s_last_rssi), buf);
}

}  // namespace ui::dashboard
