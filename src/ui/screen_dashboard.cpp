#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
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
    lv_obj_set_style_bg_color(q, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(q, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(q, 1, 0);
    lv_obj_set_style_radius(q, 8, 0);
    lv_obj_set_style_pad_all(q, 8, 0);
    lv_obj_clear_flag(q, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(q, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *h = lv_label_create(q);
    lv_label_set_text(h, header);
    lv_obj_set_style_text_color(h, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);
    return q;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);

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
    lv_obj_set_style_text_color(lbl_aws, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_aws, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_aws, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lbl_awa = lv_label_create(q1);
    lv_label_set_text(lbl_awa, "---\xC2\xB0");
    lv_obj_set_style_text_color(lbl_awa, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_awa, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_awa, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // Nav (top-right)
    lv_obj_t *q2 = make_quadrant(s_root, 1, 0, "NAV");
    lbl_sog = lv_label_create(q2);
    lv_label_set_text(lbl_sog, "--.-");
    lv_obj_set_style_text_color(lbl_sog, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_sog, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_sog, LV_ALIGN_TOP_MID, -16, 18);

    lv_obj_t *sog_unit = lv_label_create(q2);
    lv_label_set_text(sog_unit, "kn");
    lv_obj_set_style_text_color(sog_unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_font(sog_unit, &lv_font_montserrat_20, 0);
    lv_obj_align(sog_unit, LV_ALIGN_TOP_MID, 60, 38);

    lbl_cog = lv_label_create(q2);
    lv_label_set_text(lbl_cog, "COG ---\xC2\xB0");
    lv_obj_set_style_text_color(lbl_cog, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_cog, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_cog, LV_ALIGN_TOP_LEFT, 0, 92);

    lbl_hdg = lv_label_create(q2);
    lv_label_set_text(lbl_hdg, "HDG ---\xC2\xB0");
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_TOP_LEFT, 0, 122);

    lbl_pos = lv_label_create(q2);
    lv_label_set_text(lbl_pos, "---\xC2\xB0--.---'N\n---\xC2\xB0--.---'E");
    lv_obj_set_style_text_color(lbl_pos, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_pos, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Depth (bottom-left)
    lv_obj_t *q3 = make_quadrant(s_root, 0, 1, "DEPTH");
    lbl_depth = lv_label_create(q3);
    lv_label_set_text(lbl_depth, "--.-");
    lv_obj_set_style_text_color(lbl_depth, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_depth, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_depth, LV_ALIGN_CENTER, -10, -10);
    lv_obj_t *dunit = lv_label_create(q3);
    lv_label_set_text(dunit, "m");
    lv_obj_set_style_text_color(dunit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_font(dunit, &lv_font_montserrat_20, 0);
    lv_obj_align(dunit, LV_ALIGN_CENTER, 50, -10);
    lbl_temp = lv_label_create(q3);
    lv_label_set_text(lbl_temp, "water --.- \xC2\xB0""C");
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_temp, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Status (bottom-right)
    lv_obj_t *q4 = make_quadrant(s_root, 1, 1, "SYSTEM");
    lbl_batt = lv_label_create(q4);
    lv_label_set_text(lbl_batt, "--.- V");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *batt_caption = lv_label_create(q4);
    lv_label_set_text(batt_caption, "BATTERY");
    lv_obj_set_style_text_color(batt_caption, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_font(batt_caption, &lv_font_montserrat_14, 0);
    lv_obj_align(batt_caption, LV_ALIGN_TOP_MID, 0, 64);

    lbl_status = lv_label_create(q4);
    lv_label_set_text(lbl_status, "sk: -");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, -36);

    lbl_ip = lv_label_create(q4);
    lv_label_set_text(lbl_ip, "ip ---.---.---.---");
    lv_obj_set_style_text_color(lbl_ip, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_ip, LV_ALIGN_BOTTOM_LEFT, 0, -18);

    lbl_rssi = lv_label_create(q4);
    lv_label_set_text(lbl_rssi, "rssi --- dBm");
    lv_obj_set_style_text_color(lbl_rssi, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_font(lbl_rssi, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_rssi, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.aws));
        lv_label_set_text(lbl_aws, buf);
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", deg);
        lv_label_set_text(lbl_awa, buf);
        lv_obj_set_style_transform_rotation(needle, (int16_t)(deg * 10), 0);
    } else {
        // Live "pulse" so the screen reads alive even with no SK data.
        int16_t a = (int16_t)((millis() / 4) % 3600);
        lv_obj_set_style_transform_rotation(needle, a, 0);
        lv_label_set_text(lbl_awa, "no data");
    }
    if (!isnan(d.sog)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.sog));
        lv_label_set_text(lbl_sog, buf);
    }
    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "COG %.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        lv_label_set_text(lbl_cog, buf);
    }
    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "HDG %.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        lv_label_set_text(lbl_hdg, buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        format_position(d.lat, d.lon, pos_format(), buf, sizeof(buf));
        lv_label_set_text(lbl_pos, buf);
    }
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        lv_label_set_text(lbl_depth, buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "water %.1f \xC2\xB0""C", k_to_c(d.waterTemp));
        lv_label_set_text(lbl_temp, buf);
    }
    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "%.1f V", d.battVoltage);
        lv_label_set_text(lbl_batt, buf);
    } else {
        snprintf(buf, sizeof(buf), "%lu kB", (unsigned long)(ESP.getFreeHeap() / 1024));
        lv_label_set_text(lbl_batt, buf);
    }
    snprintf(buf, sizeof(buf), "sk: %s", sk::connectionStatus().c_str());
    lv_label_set_text(lbl_status, buf);
    snprintf(buf, sizeof(buf), "ip %s", net::ipString().c_str());
    lv_label_set_text(lbl_ip, buf);

    int r = net::rssi();
    uint32_t up = millis() / 1000;
    if (r != 0) {
        snprintf(buf, sizeof(buf), "rssi %d dBm  up %lu:%02lu", r, (unsigned long)(up / 60),
                 (unsigned long)(up % 60));
    } else {
        snprintf(buf, sizeof(buf), "ap mode  up %02lu:%02lu:%02lu", (unsigned long)(up / 3600),
                 (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    }
    lv_label_set_text(lbl_rssi, buf);
}

}  // namespace ui::dashboard
