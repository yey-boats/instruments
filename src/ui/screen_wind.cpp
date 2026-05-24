#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen wind dial: head-up (bow = top, 0 deg). The boat doesn't rotate;
// the wind needles do. AWA and TWA are drawn as separate needles. The dial
// is annotated with cardinal-ish ("port / stbd / bow / stern") sectors plus
// the close-hauled / gybe tactical bands.

namespace ui::wind {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_aws, *lbl_tws;
static lv_obj_t *lbl_awa, *lbl_twa;
static lv_obj_t *lbl_hdg, *lbl_cog;
static lv_obj_t *awa_needle = nullptr;
static lv_obj_t *twa_needle = nullptr;
static lv_obj_t *current_needle = nullptr;
static lv_obj_t *lbl_current = nullptr;

static constexpr int CX = 240;
static constexpr int CY = 240;
static constexpr int R_OUTER = 200;
static constexpr int R_INNER = 70;

static lv_obj_t *make_ring(lv_obj_t *p, int diameter, int border, uint32_t color) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_align(r, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(r, border, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

// Build a thin rectangle pointing UP from the dial center, pivot at center.
// Rotation in 0.1 deg units via lv_obj_set_style_transform_rotation.
static lv_obj_t *make_needle(lv_obj_t *p, int length, int width, uint32_t color) {
    lv_obj_t *n = lv_obj_create(p);
    lv_obj_set_size(n, width, length);
    lv_obj_set_style_bg_color(n, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(n, 0, 0);
    lv_obj_set_style_radius(n, width / 2, 0);
    lv_obj_set_style_pad_all(n, 0, 0);
    lv_obj_clear_flag(n, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(n, LV_OBJ_FLAG_CLICKABLE);
    // Position so the needle's tail sits at (CX, CY) and the head points up.
    lv_obj_set_pos(n, CX - width / 2, CY - length);
    // Pivot at the tail (bottom) so rotation pivots around dial center.
    lv_obj_set_style_transform_pivot_x(n, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(n, length, 0);
    return n;
}

static lv_obj_t *make_label(lv_obj_t *p, const char *txt, int x_off, int y_off,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, x_off, y_off);
    return l;
}

// Place a small text annotation at a polar position around the dial.
static void place_cardinal(lv_obj_t *p, const char *txt, int angle_deg, int radius,
                           uint32_t color) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    double a = angle_deg * M_PI / 180.0;
    int x = CX + (int)(radius * sin(a));
    int y = CY - (int)(radius * cos(a));
    lv_obj_set_pos(l, x - 10, y - 12);
}

// Tick marks every 30 deg as thin lines around the inside of R_OUTER.
static void draw_ticks(lv_obj_t *p) {
    for (int deg = 0; deg < 360; deg += 30) {
        bool major = (deg % 90) == 0;
        int len = major ? 18 : 10;
        int w = major ? 3 : 2;
        uint32_t color = major ? theme.accent : theme.grid;
        lv_obj_t *t = lv_obj_create(p);
        lv_obj_set_size(t, w, len);
        lv_obj_set_style_bg_color(t, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 1, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
        // Position tick at outer ring; the tick is vertical and rotated.
        lv_obj_set_pos(t, CX - w / 2, CY - R_OUTER);
        lv_obj_set_style_transform_pivot_x(t, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(t, R_OUTER, 0);
        lv_obj_set_style_transform_rotation(t, deg * 10, 0);
    }
}

// Soft-tinted "close hauled" arcs at ~+/-42 deg from bow as tactical hints.
static void draw_tac_sectors(lv_obj_t *p) {
    // Lay down four small dots near the close-hauled angles as visual cues.
    auto dot = [&](int deg, uint32_t c) {
        lv_obj_t *d = lv_obj_create(p);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_bg_color(d, lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_70, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        double a = deg * M_PI / 180.0;
        int x = CX + (int)((R_OUTER - 30) * sin(a));
        int y = CY - (int)((R_OUTER - 30) * cos(a));
        lv_obj_set_pos(d, x - 4, y - 4);
    };
    dot(-42, theme.port);       // close-hauled port
    dot(42, theme.starboard);   // close-hauled stbd
    dot(-150, theme.port);      // dead-down port gybe
    dot(150, theme.starboard);  // dead-down stbd gybe
}

// A small upward-pointing chevron at the dial center: the boat.
static void draw_boat(lv_obj_t *p) {
    lv_obj_t *boat = lv_obj_create(p);
    lv_obj_set_size(boat, 24, 36);
    lv_obj_set_style_bg_color(boat, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(boat, LV_OPA_60, 0);
    lv_obj_set_style_border_width(boat, 0, 0);
    lv_obj_set_style_radius(boat, 6, 0);
    lv_obj_set_style_pad_all(boat, 0, 0);
    lv_obj_clear_flag(boat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(boat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(boat, CX - 12, CY - 18);
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

    // Outer ring + inner ring
    make_ring(s_root, R_OUTER * 2, 3, theme.grid);
    make_ring(s_root, R_INNER * 2, 2, theme.grid);
    draw_ticks(s_root);
    draw_tac_sectors(s_root);
    draw_boat(s_root);

    // Cardinal-style labels for relative directions
    place_cardinal(s_root, "0", 0, R_OUTER + 18, theme.accent);
    place_cardinal(s_root, "90", 90, R_OUTER + 18, theme.starboard);
    place_cardinal(s_root, "180", 180, R_OUTER + 18, theme.accent);
    place_cardinal(s_root, "270", 270, R_OUTER + 18, theme.port);

    // Apparent wind needle (port-red), full length to outer ring
    awa_needle = make_needle(s_root, R_OUTER - 8, 6, theme.port);
    // True wind needle (white), slightly shorter / inset to differentiate
    twa_needle = make_needle(s_root, R_OUTER - 30, 4, theme.fg);
    // Current vector (dim grey), short
    current_needle = make_needle(s_root, R_INNER + 20, 3, theme.fg_dim);
    lv_obj_add_flag(current_needle, LV_OBJ_FLAG_HIDDEN);

    // Hero readouts
    lv_obj_t *header_aws = lv_label_create(s_root);
    lv_label_set_text(header_aws, "AWS");
    lv_obj_set_style_text_font(header_aws, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header_aws, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(header_aws, 12, 10);

    lbl_aws = lv_label_create(s_root);
    lv_label_set_text(lbl_aws, "--.-");
    lv_obj_set_style_text_font(lbl_aws, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_aws, lv_color_hex(theme.fg), 0);
    lv_obj_set_pos(lbl_aws, 12, 24);

    lv_obj_t *unit_aws = lv_label_create(s_root);
    lv_label_set_text(unit_aws, "kn");
    lv_obj_set_style_text_font(unit_aws, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(unit_aws, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(unit_aws, 12, 76);

    lv_obj_t *header_tws = lv_label_create(s_root);
    lv_label_set_text(header_tws, "TWS");
    lv_obj_set_style_text_font(header_tws, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header_tws, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(header_tws, LV_ALIGN_TOP_RIGHT, -12, 10);

    lbl_tws = lv_label_create(s_root);
    lv_label_set_text(lbl_tws, "--.-");
    lv_obj_set_style_text_font(lbl_tws, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_tws, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_tws, LV_ALIGN_TOP_RIGHT, -12, 24);

    lv_obj_t *unit_tws = lv_label_create(s_root);
    lv_label_set_text(unit_tws, "kn");
    lv_obj_set_style_text_font(unit_tws, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(unit_tws, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(unit_tws, LV_ALIGN_TOP_RIGHT, -12, 76);

    // AWA / TWA angle readouts at center of dial
    lbl_awa = lv_label_create(s_root);
    lv_label_set_text(lbl_awa, "AWA ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_awa, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_awa, lv_color_hex(theme.port), 0);
    lv_obj_align(lbl_awa, LV_ALIGN_CENTER, 0, 30);

    lbl_twa = lv_label_create(s_root);
    lv_label_set_text(lbl_twa, "TWA ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_twa, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_twa, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_twa, LV_ALIGN_CENTER, 0, 56);

    lbl_current = lv_label_create(s_root);
    lv_label_set_text(lbl_current, "");
    lv_obj_set_style_text_font(lbl_current, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_current, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_current, LV_ALIGN_BOTTOM_MID, 0, -36);

    // Bottom: COG + HDG side by side
    lbl_hdg = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg, "HDG ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    lbl_cog = lv_label_create(s_root);
    lv_label_set_text(lbl_cog, "COG ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_cog, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cog, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_cog, LV_ALIGN_BOTTOM_RIGHT, -12, -10);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        lv_label_set_text(lbl_aws, buf);
    }
    if (!isnan(d.tws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.tws));
        lv_label_set_text(lbl_tws, buf);
    }

    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        // Color cue: port (red) on <180, starboard (green) on >=180
        bool starboard = deg <= 180.0;
        lv_obj_set_style_bg_color(awa_needle,
                                  lv_color_hex(starboard ? theme.starboard : theme.port), 0);
        lv_obj_set_style_text_color(lbl_awa,
                                    lv_color_hex(starboard ? theme.starboard : theme.port), 0);
        snprintf(buf, sizeof(buf), "AWA %.0f\xC2\xB0%c", fmin(deg, 360 - deg),
                 starboard ? 'S' : 'P');
        lv_label_set_text(lbl_awa, buf);
        lv_obj_set_style_transform_rotation(awa_needle, (int16_t)(deg * 10), 0);
    } else {
        int16_t a = (int16_t)((millis() / 5) % 3600);
        lv_obj_set_style_transform_rotation(awa_needle, a, 0);
        lv_label_set_text(lbl_awa, "AWA --");
    }

    if (!isnan(d.twa)) {
        double deg = rad_to_deg_pos(d.twa);
        bool starboard = deg <= 180.0;
        snprintf(buf, sizeof(buf), "TWA %.0f\xC2\xB0%c", fmin(deg, 360 - deg),
                 starboard ? 'S' : 'P');
        lv_label_set_text(lbl_twa, buf);
        lv_obj_set_style_transform_rotation(twa_needle, (int16_t)(deg * 10), 0);
        lv_obj_clear_flag(twa_needle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(twa_needle, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_twa, "TWA --");
    }

    if (!isnan(d.headingTrue))
        snprintf(buf, sizeof(buf), "HDG %.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
    else
        snprintf(buf, sizeof(buf), "HDG --\xC2\xB0");
    lv_label_set_text(lbl_hdg, buf);

    if (!isnan(d.cogTrue))
        snprintf(buf, sizeof(buf), "COG %.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
    else
        snprintf(buf, sizeof(buf), "COG --\xC2\xB0");
    lv_label_set_text(lbl_cog, buf);

    // Current vector + label not yet sourced; kept hidden until we expose
    // environment.current.{setTrue,drift} on sk::Data.
}

}  // namespace ui::wind
