#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen nav page: SOG dominates, COG/HDG split below it, lat/lon in
// DDM (configurable), bottom strip with depth/temp.

namespace ui::nav {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_sog;
static lv_obj_t *lbl_cog_value, *lbl_hdg_value;
static lv_obj_t *lbl_pos;
static lv_obj_t *lbl_depth_small, *lbl_temp_small, *lbl_awa_small;

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

static lv_obj_t *caption(lv_obj_t *parent, const char *txt, lv_align_t align, int xo, int yo) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(l, align, xo, yo);
    return l;
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

    // Hero: SOG card spans the top half
    lv_obj_t *hero = make_card(s_root, 4, 4, LCD_W - 8, 200);
    caption(hero, "SOG", LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_sog = lv_label_create(hero);
    lv_label_set_text(lbl_sog, "--.-");
    lv_obj_set_style_text_font(lbl_sog, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_sog, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_sog, LV_ALIGN_CENTER, -30, 12);

    // Visual cue: scale the SOG to feel "huger" via spacing + a kn tag
    lv_obj_t *unit = lv_label_create(hero);
    lv_label_set_text(unit, "kn");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -20, 12);

    // Two course cards (COG / HDG)
    lv_obj_t *card_cog = make_card(s_root, 4, 208, 232, 92);
    caption(card_cog, "COG", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_cog_value = lv_label_create(card_cog);
    lv_label_set_text(lbl_cog_value, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_cog_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_cog_value, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_cog_value, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *card_hdg = make_card(s_root, 244, 208, 232, 92);
    caption(card_hdg, "HDG", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_hdg_value = lv_label_create(card_hdg);
    lv_label_set_text(lbl_hdg_value, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_hdg_value, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_hdg_value, LV_ALIGN_CENTER, 0, 6);

    // Position card
    lv_obj_t *card_pos = make_card(s_root, 4, 304, LCD_W - 8, 116);
    caption(card_pos, "POSITION", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_pos = lv_label_create(card_pos);
    lv_label_set_text(lbl_pos, "---\xC2\xB0--.---'N\n---\xC2\xB0--.---'E");
    lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_pos, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_pos, LV_ALIGN_CENTER, 0, 8);

    // Bottom strip: depth, water temp, AWA (compact, single line)
    lv_obj_t *strip = make_card(s_root, 4, 424, LCD_W - 8, 52);
    lv_obj_set_style_pad_all(strip, 4, 0);

    lbl_depth_small = lv_label_create(strip);
    lv_label_set_text(lbl_depth_small, "DEPTH --.- m");
    lv_obj_set_style_text_font(lbl_depth_small, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_depth_small, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_depth_small, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_temp_small = lv_label_create(strip);
    lv_label_set_text(lbl_temp_small, "H2O --.-\xC2\xB0""C");
    lv_obj_set_style_text_font(lbl_temp_small, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_temp_small, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_temp_small, LV_ALIGN_CENTER, 0, 0);

    lbl_awa_small = lv_label_create(strip);
    lv_label_set_text(lbl_awa_small, "AWA ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_awa_small, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_awa_small, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_awa_small, LV_ALIGN_RIGHT_MID, -4, 0);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    if (!isnan(d.sog)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.sog));
        lv_label_set_text(lbl_sog, buf);
    }
    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        lv_label_set_text(lbl_cog_value, buf);
    }
    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        lv_label_set_text(lbl_hdg_value, buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        format_position(d.lat, d.lon, pos_format(), buf, sizeof(buf));
        lv_label_set_text(lbl_pos, buf);
    }
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "DEPTH %.1f m", d.depth);
        lv_label_set_text(lbl_depth_small, buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "H2O %.1f\xC2\xB0""C", k_to_c(d.waterTemp));
        lv_label_set_text(lbl_temp_small, buf);
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        snprintf(buf, sizeof(buf), "AWA %.0f\xC2\xB0%c", fmin(deg, 360 - deg),
                 starboard ? 'S' : 'P');
        lv_label_set_text(lbl_awa_small, buf);
        lv_obj_set_style_text_color(lbl_awa_small,
                                    lv_color_hex(starboard ? theme.starboard : theme.port), 0);
    }
}

}  // namespace ui::nav
