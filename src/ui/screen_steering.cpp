#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen steering page. Head-up compass; current HDG marks the top
// (12 o'clock). A "heading bug" indicates CTS (course to steer); steer to
// align bow with bug. XTE bar at the bottom: triangle slides along a
// horizontal scale, color-coded by track side.

namespace ui::steering {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *bug = nullptr;
static lv_obj_t *bow = nullptr;
static lv_obj_t *xte_indicator = nullptr;
static lv_obj_t *xte_scale = nullptr;
static lv_obj_t *lbl_hdg, *lbl_cts, *lbl_xte, *lbl_dtw, *lbl_btw, *lbl_ap;

static constexpr int CX = 240;
static constexpr int CY = 220;
static constexpr int R_OUTER = 170;

static lv_obj_t *make_ring(lv_obj_t *p, int diameter, int border, uint32_t color) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, CY - diameter / 2);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(r, border, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static lv_obj_t *make_marker(lv_obj_t *p, int w, int h, uint32_t color) {
    lv_obj_t *m = lv_obj_create(p);
    lv_obj_set_size(m, w, h);
    lv_obj_set_style_bg_color(m, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, w / 2, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(m, CX - w / 2, CY - R_OUTER);
    lv_obj_set_style_transform_pivot_x(m, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(m, R_OUTER, 0);
    return m;
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

    // Title
    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "STEERING");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Compass ring
    make_ring(s_root, R_OUTER * 2, 3, theme.grid);

    // Fixed bow indicator at top
    bow = lv_obj_create(s_root);
    lv_obj_set_size(bow, 4, 32);
    lv_obj_set_style_bg_color(bow, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(bow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bow, 0, 0);
    lv_obj_set_style_radius(bow, 2, 0);
    lv_obj_set_style_pad_all(bow, 0, 0);
    lv_obj_clear_flag(bow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bow, CX - 2, CY - R_OUTER - 16);

    // Heading bug (rotates to delta(CTS - HDG))
    bug = make_marker(s_root, 16, 16, theme.warn);

    // HDG (huge, center)
    lbl_hdg = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_TOP_MID, 0, CY - 30);

    lv_obj_t *hdg_cap = lv_label_create(s_root);
    lv_label_set_text(hdg_cap, "HDG");
    lv_obj_set_style_text_font(hdg_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hdg_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(hdg_cap, LV_ALIGN_TOP_MID, 0, CY + 30);

    // CTS / BTW small
    lbl_cts = lv_label_create(s_root);
    lv_label_set_text(lbl_cts, "CTS ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_cts, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cts, lv_color_hex(theme.warn), 0);
    lv_obj_align(lbl_cts, LV_ALIGN_TOP_LEFT, 12, 40);

    lbl_btw = lv_label_create(s_root);
    lv_label_set_text(lbl_btw, "BTW ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_btw, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_btw, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_btw, LV_ALIGN_TOP_RIGHT, -12, 40);

    lbl_dtw = lv_label_create(s_root);
    lv_label_set_text(lbl_dtw, "DTW --- nm");
    lv_obj_set_style_text_font(lbl_dtw, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_dtw, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_dtw, LV_ALIGN_TOP_LEFT, 12, 68);

    // Autopilot strip
    lbl_ap = lv_label_create(s_root);
    lv_label_set_text(lbl_ap, "AP standby");
    lv_obj_set_style_text_font(lbl_ap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_ap, LV_ALIGN_TOP_RIGHT, -12, 68);

    // XTE bar at the bottom
    xte_scale = lv_obj_create(s_root);
    lv_obj_set_size(xte_scale, 400, 28);
    lv_obj_align(xte_scale, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_color(xte_scale, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(xte_scale, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(xte_scale, 1, 0);
    lv_obj_set_style_radius(xte_scale, 4, 0);
    lv_obj_set_style_pad_all(xte_scale, 0, 0);
    lv_obj_clear_flag(xte_scale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(xte_scale, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Center notch
    lv_obj_t *notch = lv_obj_create(xte_scale);
    lv_obj_set_size(notch, 2, 28);
    lv_obj_align(notch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(notch, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(notch, LV_OPA_70, 0);
    lv_obj_set_style_border_width(notch, 0, 0);
    lv_obj_set_style_pad_all(notch, 0, 0);
    lv_obj_clear_flag(notch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(notch, LV_OBJ_FLAG_CLICKABLE);

    // Indicator (slides left/right)
    xte_indicator = lv_obj_create(xte_scale);
    lv_obj_set_size(xte_indicator, 12, 24);
    lv_obj_align(xte_indicator, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(xte_indicator, lv_color_hex(theme.warn), 0);
    lv_obj_set_style_bg_opa(xte_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(xte_indicator, 0, 0);
    lv_obj_set_style_radius(xte_indicator, 6, 0);
    lv_obj_set_style_pad_all(xte_indicator, 0, 0);
    lv_obj_clear_flag(xte_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(xte_indicator, LV_OBJ_FLAG_CLICKABLE);

    lbl_xte = lv_label_create(s_root);
    lv_label_set_text(lbl_xte, "XTE --- m");
    lv_obj_set_style_text_font(lbl_xte, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_xte, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_xte, LV_ALIGN_BOTTOM_MID, 0, -12);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    double hdg_deg = NAN, cts_deg = NAN;
    if (!isnan(d.headingTrue)) {
        hdg_deg = rad_to_deg_pos(d.headingTrue);
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", hdg_deg);
        lv_label_set_text(lbl_hdg, buf);
    }
    if (!isnan(d.cts)) {
        cts_deg = rad_to_deg_pos(d.cts);
        snprintf(buf, sizeof(buf), "CTS %03.0f\xC2\xB0", cts_deg);
        lv_label_set_text(lbl_cts, buf);
    }
    if (!isnan(d.btw)) {
        snprintf(buf, sizeof(buf), "BTW %03.0f\xC2\xB0", rad_to_deg_pos(d.btw));
        lv_label_set_text(lbl_btw, buf);
    }
    if (!isnan(d.dtw)) {
        if (d.dtw >= 1852.0)
            snprintf(buf, sizeof(buf), "DTW %.2f nm", d.dtw / 1852.0);
        else
            snprintf(buf, sizeof(buf), "DTW %.0f m", d.dtw);
        lv_label_set_text(lbl_dtw, buf);
    }

    // Heading bug position: delta = CTS - HDG (head-up)
    if (!isnan(hdg_deg) && !isnan(cts_deg)) {
        double delta = cts_deg - hdg_deg;
        while (delta > 180) delta -= 360;
        while (delta < -180) delta += 360;
        lv_obj_set_style_transform_rotation(bug, (int16_t)(delta * 10), 0);
        lv_obj_clear_flag(bug, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bug, LV_OBJ_FLAG_HIDDEN);
    }

    // XTE: clamp at +-200m for display, map to scale half-width (-180..180 px)
    if (!isnan(d.xte)) {
        double clamped = d.xte;
        if (clamped > 200) clamped = 200;
        if (clamped < -200) clamped = -200;
        int x = (int)(clamped / 200.0 * 180);
        lv_obj_align(xte_indicator, LV_ALIGN_CENTER, x, 0);
        // Color: warn if > 50 m, alarm if > 100 m
        uint32_t c = theme.good;
        double abs_xte = fabs(d.xte);
        if (abs_xte > 100)
            c = theme.alarm;
        else if (abs_xte > 50)
            c = theme.warn;
        lv_obj_set_style_bg_color(xte_indicator, lv_color_hex(c), 0);
        const char *side = d.xte > 0 ? "STBD" : (d.xte < 0 ? "PORT" : "");
        snprintf(buf, sizeof(buf), "XTE %.0f m %s", fabs(d.xte), side);
        lv_label_set_text(lbl_xte, buf);
    } else {
        lv_label_set_text(lbl_xte, "XTE -- m");
        lv_obj_align(xte_indicator, LV_ALIGN_CENTER, 0, 0);
    }

    // AP state strip
    if (d.apState[0]) {
        if (!isnan(d.apTargetHdg))
            snprintf(buf, sizeof(buf), "AP %s -> %03.0f\xC2\xB0", d.apState,
                     rad_to_deg_pos(d.apTargetHdg));
        else
            snprintf(buf, sizeof(buf), "AP %s", d.apState);
        lv_label_set_text(lbl_ap, buf);
        bool engaged = (strcmp(d.apState, "auto") == 0 || strcmp(d.apState, "wind") == 0 ||
                        strcmp(d.apState, "route") == 0);
        lv_obj_set_style_text_color(lbl_ap,
                                    lv_color_hex(engaged ? theme.good : theme.fg_dim), 0);
    } else {
        lv_label_set_text(lbl_ap, "AP -");
    }
}

}  // namespace ui::steering
