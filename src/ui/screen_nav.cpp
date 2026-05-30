#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
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
    style_panel(c, theme.accent);
    return c;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *txt, lv_align_t align, int xo, int yo) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    style_caption(l);
    lv_obj_align(l, align, xo, yo);
    return l;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    style_screen(s_root);

    // Hero: SOG card spans the top half
    lv_obj_t *hero = make_card(s_root, 4, 4, LCD_W - 8, 200);
    panel_accent(hero, theme.accent);
    caption(hero, "SOG", LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_sog = lv_label_create(hero);
    lv_label_set_text(lbl_sog, "--.-");
    style_value(lbl_sog, &lv_font_montserrat_48, theme.accent);
    lv_obj_align(lbl_sog, LV_ALIGN_CENTER, -30, 12);

    // Visual cue: scale the SOG to feel "huger" via spacing + a kn tag
    lv_obj_t *unit = lv_label_create(hero);
    lv_label_set_text(unit, "kn");
    style_value(unit, &lv_font_montserrat_28, theme.fg_dim);
    lv_obj_align(unit, LV_ALIGN_RIGHT_MID, -20, 12);

    // Two course cards (COG / HDG)
    lv_obj_t *card_cog = make_card(s_root, 4, 208, 232, 92);
    panel_accent(card_cog, theme.grid);
    caption(card_cog, "COG", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_cog_value = lv_label_create(card_cog);
    lv_label_set_text(lbl_cog_value, "---\xC2\xB0");
    style_value(lbl_cog_value, &lv_font_montserrat_48, theme.fg);
    lv_obj_align(lbl_cog_value, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *card_hdg = make_card(s_root, 244, 208, 232, 92);
    panel_accent(card_hdg, theme.accent);
    caption(card_hdg, "HDG", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_hdg_value = lv_label_create(card_hdg);
    lv_label_set_text(lbl_hdg_value, "---\xC2\xB0");
    style_value(lbl_hdg_value, &lv_font_montserrat_48, theme.accent);
    lv_obj_align(lbl_hdg_value, LV_ALIGN_CENTER, 0, 6);

    // Position card
    lv_obj_t *card_pos = make_card(s_root, 4, 304, LCD_W - 8, 116);
    panel_accent(card_pos, theme.good);
    caption(card_pos, "POSITION", LV_ALIGN_TOP_LEFT, 0, 0);
    lbl_pos = lv_label_create(card_pos);
    lv_label_set_text(lbl_pos, "---\xC2\xB0--.---'N\n---\xC2\xB0--.---'E");
    style_value(lbl_pos, &lv_font_montserrat_28, theme.fg);
    lv_obj_align(lbl_pos, LV_ALIGN_CENTER, 0, 8);

    // Bottom strip: depth, water temp, AWA (compact, single line)
    lv_obj_t *strip = make_card(s_root, 4, 424, LCD_W - 8, 52);
    lv_obj_set_style_pad_all(strip, 4, 0);

    lbl_depth_small = lv_label_create(strip);
    lv_label_set_text(lbl_depth_small, "DEPTH --.- m");
    style_value(lbl_depth_small, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_depth_small, LV_ALIGN_LEFT_MID, 4, 0);

    lbl_temp_small = lv_label_create(strip);
    lv_label_set_text(lbl_temp_small, "H2O --.-\xC2\xB0"
                                      "C");
    style_value(lbl_temp_small, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_temp_small, LV_ALIGN_CENTER, 0, 0);

    lbl_awa_small = lv_label_create(strip);
    lv_label_set_text(lbl_awa_small, "AWA ---\xC2\xB0");
    style_value(lbl_awa_small, &lv_font_montserrat_20, theme.fg);
    lv_obj_align(lbl_awa_small, LV_ALIGN_RIGHT_MID, -4, 0);

    return s_root;
}

// Dirty-value caches per docs/specs/09: skip lv_label_set_text /
// lv_obj_set_style_text_color when the displayed value hasn't changed,
// so the partial-render path doesn't re-flush untouched labels.
static char s_last_sog[16] = {(char)0xFF};
static char s_last_cog[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_pos[64] = {(char)0xFF};
static char s_last_depth[24] = {(char)0xFF};
static char s_last_temp[24] = {(char)0xFF};
static char s_last_awa[24] = {(char)0xFF};
static uint32_t s_last_awa_color = 0xFFFFFFFF;

void refresh() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    if (!isnan(d.sog)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.sog));
        set_text_if_changed(lbl_sog, s_last_sog, sizeof(s_last_sog), buf);
    }
    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        set_text_if_changed(lbl_cog_value, s_last_cog, sizeof(s_last_cog), buf);
    }
    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        format_position(d.lat, d.lon, pos_format(), buf, sizeof(buf));
        set_text_if_changed(lbl_pos, s_last_pos, sizeof(s_last_pos), buf);
    }
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "DEPTH %.1f m", d.depth);
        set_text_if_changed(lbl_depth_small, s_last_depth, sizeof(s_last_depth), buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf),
                 "H2O %.1f\xC2\xB0"
                 "C",
                 k_to_c(d.waterTemp));
        set_text_if_changed(lbl_temp_small, s_last_temp, sizeof(s_last_temp), buf);
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        snprintf(buf, sizeof(buf), "AWA %.0f\xC2\xB0%c", fmin(deg, 360 - deg),
                 starboard ? 'S' : 'P');
        set_text_if_changed(lbl_awa_small, s_last_awa, sizeof(s_last_awa), buf);
        set_text_color_if_changed(lbl_awa_small, &s_last_awa_color,
                                  starboard ? theme.starboard : theme.port);
    }
}

}  // namespace ui::nav
