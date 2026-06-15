#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Wind-steering screen: a bow-up sailing arc that zooms into the working zone.
// Upwind it shows the TOP semicircle (the wind is forward); downwind it flips to
// the BOTTOM semicircle (the wind is aft). A red NO-GO wedge marks the zone you
// can't point into, with green LAYLINE marks at its edges, and a bright wind
// marker shows the live true-wind angle — steer so the wind marker sits on a
// layline to sail the optimal close-hauled (or running) angle. The no-go width
// and laylines come from the SignalK polar beat/gybe angles (so they shift with
// wind speed), falling back to an empirical estimate when no polar is present.
//
// Angle convention: theta = true wind angle off the bow, +starboard. On screen
// the bow is at the top; LVGL angle = 270 + theta (270 = 12 o'clock).

namespace ui::wind_steer {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *band_top = nullptr;   // white band, upper semicircle (upwind)
static lv_obj_t *band_bot = nullptr;   // white band, lower semicircle (downwind)
static lv_obj_t *nogo = nullptr;       // red no-go wedge (dynamic angles)
static lv_obj_t *target_a = nullptr;   // green target sector, starboard (dynamic)
static lv_obj_t *target_b = nullptr;   // green target sector, port (dynamic)
static lv_obj_t *wind_mark = nullptr;  // live true-wind marker (rotating)
static lv_obj_t *ticks_top = nullptr;  // tick+label group for the upper half
static lv_obj_t *ticks_bot = nullptr;  // tick+label group for the lower half
static lv_obj_t *lbl_twa, *lbl_twd, *lbl_status, *lbl_src;

static constexpr int CX = LCD_W / 2;
static constexpr int CY = LCD_H / 2;
static constexpr int SHORT = (LCD_W < LCD_H ? LCD_W : LCD_H);
static constexpr int R = SHORT / 2 - 28;
static constexpr int BAND_W = 52;  // white-band thickness, matching the AP compass

static double norm360(double d) {
    while (d < 0)
        d += 360;
    while (d >= 360)
        d -= 360;
    return d;
}

// A fixed colored band arc (white rim) for one half. a0/a1 in LVGL degrees.
static lv_obj_t *band_arc(int a0, int a1, int radius, int width, uint32_t color, lv_opa_t opa) {
    lv_obj_t *arc = lv_arc_create(s_root);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    int d = radius * 2;
    lv_obj_set_size(arc, d, d);
    lv_obj_set_pos(arc, CX - radius, CY - radius);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, a0, a1);
    lv_arc_set_angles(arc, a0, a1);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    return arc;
}

// A rim marker: a transparent holder pivoting at the dial centre with a short
// visible blade at the rim. Rotating the holder sweeps the blade to a wind
// angle (rotation 0 = bow/top). `blade_w`/`blade_h` size the colored tip.
static lv_obj_t *rim_marker(int blade_w, int blade_h, uint32_t color, bool triangle) {
    lv_obj_t *h = lv_obj_create(s_root);
    lv_obj_set_size(h, blade_w + 8, R);
    lv_obj_set_pos(h, CX - (blade_w + 8) / 2, CY - R);
    lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(h, (blade_w + 8) / 2, 0);
    lv_obj_set_style_transform_pivot_y(h, R, 0);  // pivot at the dial centre

    if (triangle) {
        lv_obj_t *t = lv_label_create(h);
        lv_label_set_text(t, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, -6);
    }
    lv_obj_t *blade = lv_obj_create(h);
    lv_obj_set_size(blade, blade_w, blade_h);
    lv_obj_set_style_bg_color(blade, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(blade, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(blade, 0, 0);
    lv_obj_set_style_radius(blade, 2, 0);
    lv_obj_set_style_pad_all(blade, 0, 0);
    lv_obj_clear_flag(blade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(blade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(blade, LV_ALIGN_TOP_MID, 0, triangle ? 22 : 0);
    return h;
}

// Build a tick + angle-label group for one half (top = upwind / bow region).
// Ticks every 15 deg, numbers every 30 deg showing the angle off the bow.
static lv_obj_t *build_ticks(bool top) {
    lv_obj_t *g = lv_obj_create(s_root);
    lv_obj_set_size(g, LCD_W, LCD_H);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    // theta off bow: top half spans -90..+90, bottom half spans +90..+270.
    // ~220 deg working zone: a bit past the beam on each half. Range is aligned
    // to multiples of 15 so the 30-deg majors (0/30/60/90) land on real ticks.
    int lo = top ? -105 : 75;
    int hi = top ? 105 : 285;
    for (int th = lo; th <= hi; th += 15) {
        bool major = (((th % 30) + 360) % 30) == 0;
        // Tick sizes match the autopilot compass (major 12x3, minor 7x2).
        lv_obj_t *t = lv_obj_create(g);
        int tw = major ? 3 : 2;
        int rim = R - 4;  // just inside the band's outer edge
        lv_obj_set_size(t, tw, major ? 12 : 7);
        lv_obj_set_style_bg_color(t, lv_color_hex(major ? 0x16222f : 0x5a6b78), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_transform_pivot_x(t, tw / 2, 0);
        lv_obj_set_style_transform_pivot_y(t, rim, 0);
        lv_obj_set_pos(t, CX - tw / 2, CY - rim);
        lv_obj_set_style_transform_rotation(t, ((th % 360) + 360) % 360 * 10, 0);

        if (major) {
            int mag = abs(th) % 360;
            if (mag > 180) mag = 360 - mag;  // angle off bow, 0..180
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", mag);
            lv_obj_t *l = lv_label_create(g);
            lv_label_set_text(l, nb);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0x16222f), 0);
            lv_obj_set_width(l, 44);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            double a = th * M_PI / 180.0;
            int x = CX + (int)((R - BAND_W / 2) * sin(a));  // centred on the band
            int y = CY - (int)((R - BAND_W / 2) * cos(a));
            lv_obj_set_pos(l, x - 22, y - 13);
        }
    }
    return g;
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

    // ~220 deg bow-up / stern-up bands. The band is a thick white rim (matching
    // the autopilot compass line sizes); the NO-GO (red) and TARGET (green)
    // sectors fill it at full width so they read as proper colored sectors.
    band_top = band_arc(160, 20, R, BAND_W, theme.arc_band, LV_OPA_COVER);
    band_bot = band_arc(340, 200, R, BAND_W, theme.arc_band, LV_OPA_COVER);
    lv_obj_add_flag(band_bot, LV_OBJ_FLAG_HIDDEN);

    nogo = band_arc(252, 288, R, BAND_W, theme.alarm, LV_OPA_COVER);  // dynamic in refresh

    // Green target sectors: the "sail here" band just outside the no-go each side.
    target_a = band_arc(300, 314, R, BAND_W, theme.good, LV_OPA_COVER);
    target_b = band_arc(226, 240, R, BAND_W, theme.good, LV_OPA_COVER);

    ticks_top = build_ticks(true);
    ticks_bot = build_ticks(false);
    lv_obj_add_flag(ticks_bot, LV_OBJ_FLAG_HIDDEN);

    wind_mark = rim_marker(8, 44, 0x2bd4e8, true);  // bright cyan wind marker

    // Centre readouts.
    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "TRUE WIND");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -54);

    lbl_twa = lv_label_create(s_root);
    lv_label_set_text(lbl_twa, "--\xC2\xB0");
    lv_obj_set_style_text_font(lbl_twa, &font_xl_64, 0);
    lv_obj_set_style_text_color(lbl_twa, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_text_align(lbl_twa, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_twa, LV_ALIGN_CENTER, 0, -8);

    lbl_twd = lv_label_create(s_root);
    lv_label_set_text(lbl_twd, "TWD --\xC2\xB0");
    lv_obj_set_style_text_font(lbl_twd, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_twd, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_twd, LV_ALIGN_CENTER, 0, 40);

    lbl_status = lv_label_create(s_root);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(theme.good), 0);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 70);

    lbl_src = lv_label_create(s_root);
    lv_label_set_text(lbl_src, "");
    lv_obj_set_style_text_font(lbl_src, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_src, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_src, LV_ALIGN_BOTTOM_MID, 0, -8);

    return s_root;
}

// ---- refresh ----

static char s_last_twa[12] = {(char)0xFF};
static char s_last_twd[16] = {(char)0xFF};
static char s_last_status[20] = {(char)0xFF};
static char s_last_src[20] = {(char)0xFF};
static int16_t s_last_wind_rot = INT16_MIN;
static int8_t s_last_up = -1;

static int16_t rot10(double deg) {
    int16_t r = (int16_t)(lround(deg) * 10);
    while (r < 0)
        r += 3600;
    while (r >= 3600)
        r -= 3600;
    return r;
}

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[24];

    double twa_raw = isnan(d.twa) ? NAN : rad_to_deg_pos(d.twa);  // 0..360, +stbd
    double twa_signed = isnan(twa_raw) ? NAN : (twa_raw <= 180.0 ? twa_raw : twa_raw - 360.0);
    bool stbd = !isnan(twa_signed) && twa_signed >= 0;
    double twa_mag = isnan(twa_signed) ? NAN : fabs(twa_signed);
    bool up = isnan(twa_mag) ? true : (twa_mag <= 90.0);

    double hdg = isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
    double twd = (!isnan(hdg) && !isnan(twa_raw)) ? norm360(hdg + twa_raw) : NAN;
    double beat = isnan(d.beatAngle) ? NAN : rad_to_deg_pos(d.beatAngle);
    double gybe = isnan(d.gybeAngle) ? NAN : rad_to_deg_pos(d.gybeAngle);
    bool have_polar = (up && !isnan(beat)) || (!up && !isnan(gybe));

    // Optimal angle off the bow for the current point of sail; empirical default.
    double opt = up ? (isnan(beat) ? 45.0 : beat) : (isnan(gybe) ? 150.0 : gybe);

    // Show the working-half band (top when upwind, bottom when downwind).
    if ((int8_t)up != s_last_up) {
        s_last_up = (int8_t)up;
        lv_obj_t *show_band = up ? band_top : band_bot;
        lv_obj_t *hide_band = up ? band_bot : band_top;
        lv_obj_t *show_ticks = up ? ticks_top : ticks_bot;
        lv_obj_t *hide_ticks = up ? ticks_bot : ticks_top;
        lv_obj_clear_flag(show_band, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hide_band, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(show_ticks, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hide_ticks, LV_OBJ_FLAG_HIDDEN);
    }

    // No-go wedge: centred on the bow (upwind) / stern (downwind), half-width =
    // the optimal angle (upwind) or (180 - optimal) (downwind). LVGL angles.
    double center_lvgl = up ? 270.0 : 90.0;
    double half = up ? opt : (180.0 - opt);
    if (half < 4) half = 4;
    if (half > 88) half = 88;
    lv_arc_set_bg_angles(nogo, (int)norm360(center_lvgl - half), (int)norm360(center_lvgl + half));
    lv_arc_set_angles(nogo, (int)norm360(center_lvgl - half), (int)norm360(center_lvgl + half));

    // Green target sectors: the "sail here" band just outside the no-go on each
    // side. With a polar present it hugs the optimal beat/gybe angle; the band
    // width conveys the working tolerance (a future polar-table fetch can size it
    // from the beat angle across the working TWS range). Upwind the band is wider
    // than optimal (footing); downwind it is tighter than optimal (hotter).
    double band = 12.0;
    double t1 = up ? opt : (opt - band);
    double t2 = up ? (opt + band) : opt;
    lv_arc_set_bg_angles(target_a, (int)norm360(270 + t1), (int)norm360(270 + t2));
    lv_arc_set_angles(target_a, (int)norm360(270 + t1), (int)norm360(270 + t2));
    lv_arc_set_bg_angles(target_b, (int)norm360(270 - t2), (int)norm360(270 - t1));
    lv_arc_set_angles(target_b, (int)norm360(270 - t2), (int)norm360(270 - t1));

    // Wind marker at the live true-wind angle.
    if (!isnan(twa_signed)) {
        set_rot_if_changed(wind_mark, &s_last_wind_rot, rot10(twa_signed));
        lv_obj_clear_flag(wind_mark, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0%c", twa_mag, stbd ? 'S' : 'P');
        set_text_if_changed(lbl_twa, s_last_twa, sizeof(s_last_twa), buf);
    } else {
        lv_obj_add_flag(wind_mark, LV_OBJ_FLAG_HIDDEN);
        set_text_if_changed(lbl_twa, s_last_twa, sizeof(s_last_twa), "--\xC2\xB0");
    }

    if (!isnan(twd)) {
        snprintf(buf, sizeof(buf), "TWD %03.0f\xC2\xB0", twd);
        set_text_if_changed(lbl_twd, s_last_twd, sizeof(s_last_twd), buf);
    } else {
        set_text_if_changed(lbl_twd, s_last_twd, sizeof(s_last_twd), "TWD --\xC2\xB0");
    }

    // Status: how far off the optimal angle (steer to close the gap).
    if (!isnan(twa_mag)) {
        double delta = twa_mag - opt;  // +ve = wider than optimal, -ve = pinching
        if (fabs(delta) <= 3.0)
            snprintf(buf, sizeof(buf), "ON %s", up ? "LAYLINE" : "GYBE");
        else if (delta < 0)
            snprintf(buf, sizeof(buf), "%s %.0f\xC2\xB0", up ? "PINCHING" : "HIGH", -delta);
        else
            snprintf(buf, sizeof(buf), "%s %.0f\xC2\xB0", up ? "WIDE" : "DEEP", delta);
        set_text_if_changed(lbl_status, s_last_status, sizeof(s_last_status), buf);
    } else {
        set_text_if_changed(lbl_status, s_last_status, sizeof(s_last_status), "");
    }

    set_text_if_changed(lbl_src, s_last_src, sizeof(s_last_src),
                        have_polar ? "polars: SignalK" : "polars: estimated");
}

}  // namespace ui::wind_steer
