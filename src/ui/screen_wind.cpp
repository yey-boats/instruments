#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen wind dial. Visual language inspired by
// OpenMarineSystems/marine-nav-compass-card (rotating heading bezel,
// red/green close-hauled arcs, separate apparent / true wind markers,
// tide vector at center). Adapted to LVGL on a 480x480 panel.
//
// Coordinate conventions:
//   - Bow points up (y -).
//   - SignalK angleApparent / angleTrueWater are bow-relative radians, so
//     marker rotation = those angles directly (no heading subtraction).
//   - Heading rotates the OUTER bezel only (-heading_deg) so the cardinal
//     letter matching the boat's track sits at the top.
//   - Current.setTrue is a compass bearing; the tide arrow rotates by
//     (setTrue - heading) to be rendered bow-relative.

namespace ui::wind {

static lv_obj_t *s_root = nullptr;

// Rotating outer bezel (cardinals + ticks)
static lv_obj_t *bezel = nullptr;

// Static markers (relative to the boat)
static lv_obj_t *awa_marker = nullptr;
static lv_obj_t *twa_marker = nullptr;
static lv_obj_t *tide_arrow = nullptr;
static lv_obj_t *waypoint_marker = nullptr;

// Hero readouts
static lv_obj_t *lbl_aws_value, *lbl_tws_value;
static lv_obj_t *lbl_awa_value, *lbl_twa_value;
static lv_obj_t *lbl_hdg_value, *lbl_cog_value;
static lv_obj_t *lbl_tide_speed;

// Geometry derived from LCD_W/LCD_H so the same source compiles for
// 480x480 sunton, 800x480 waveshare-4.3/5/7, and 1024x600 waveshare-5/7B.
// Radii scale with the shorter axis; centered on screen. The 22 px
// margin matches the safe-zone band on the dashboards.
static constexpr int CX = LCD_W / 2;
static constexpr int CY = LCD_H / 2;
static constexpr int DIAL_SHORTSIDE = (LCD_W < LCD_H ? LCD_W : LCD_H);
static constexpr int R_BEZEL = DIAL_SHORTSIDE / 2 - 22;  // 480->218, 600->278
static constexpr int R_FACE = R_BEZEL - 28;
static constexpr int R_CLOSEHAULED = R_BEZEL - 43;
static constexpr int R_MARKER = R_BEZEL - 18;

// ---- helpers -----------------------------------------------------------

// Position `o` so its tail sits `distance_above_center` above (cx, cy)
// in the PARENT'S coordinate system, with rotation pivot at the tail.
// Rotating `o` then sweeps it around (cx, cy).
static void apply_pivot_at(lv_obj_t *o, int cx, int cy, int half_w, int distance_above_center) {
    lv_obj_set_pos(o, cx - half_w, cy - distance_above_center);
    lv_obj_set_style_transform_pivot_x(o, half_w, 0);
    lv_obj_set_style_transform_pivot_y(o, distance_above_center, 0);
}
static void apply_pivot_center(lv_obj_t *o, int half_w, int distance_above_center) {
    apply_pivot_at(o, CX, CY, half_w, distance_above_center);
}

static lv_obj_t *make_label_at_polar_at(lv_obj_t *parent, int cx, int cy, const char *txt,
                                        int angle_deg, int radius, const lv_font_t *font,
                                        uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    double a = angle_deg * M_PI / 180.0;
    int x = cx + (int)(radius * sin(a));
    int y = cy - (int)(radius * cos(a));
    int hw = 14;  // half-width estimate (LVGL labels don't auto-center)
    int hh = 10;
    lv_obj_set_pos(l, x - hw, y - hh);
    return l;
}
static lv_obj_t *make_label_at_polar(lv_obj_t *parent, const char *txt, int angle_deg, int radius,
                                     const lv_font_t *font, uint32_t color) {
    return make_label_at_polar_at(parent, CX, CY, txt, angle_deg, radius, font, color);
}

// Build a "tick rectangle" sticking inward from the bezel rim, rotating
// around (cx, cy) in the parent's coordinate system.
static lv_obj_t *make_tick_at(lv_obj_t *parent, int cx, int cy, int angle_deg, int len, int width,
                              uint32_t color) {
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_size(t, width, len);
    lv_obj_set_style_bg_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 1, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_at(t, cx, cy, width / 2, R_BEZEL - 4);
    lv_obj_set_style_transform_rotation(t, angle_deg * 10, 0);
    return t;
}

static lv_obj_t *make_ring_at(lv_obj_t *p, int cx, int cy, int diameter, int border, uint32_t color,
                              int opa = LV_OPA_COVER) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_set_pos(r, cx - diameter / 2, cy - diameter / 2);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(r, opa, 0);
    lv_obj_set_style_border_width(r, border, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}
static lv_obj_t *make_ring(lv_obj_t *p, int diameter, int border, uint32_t color,
                           int opa = LV_OPA_COVER) {
    return make_ring_at(p, CX, CY, diameter, border, color, opa);
}

// ---- bezel -------------------------------------------------------------

static void build_bezel(lv_obj_t *parent) {
    // The bezel is a transparent group sized to enclose the rim; the
    // cardinal labels + ticks live inside it. Rotating this group rotates
    // them all together around the dial center.
    bezel = lv_obj_create(parent);
    int size = R_BEZEL * 2 + 12;
    int bcx = size / 2;  // bezel's LOCAL center - children must use this,
    int bcy = size / 2;  // not CX/CY which are screen-absolute.
    lv_obj_set_size(bezel, size, size);
    lv_obj_set_pos(bezel, CX - bcx, CY - bcy);  // bezel's local center == screen's (CX, CY)
    lv_obj_set_style_bg_opa(bezel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bezel, 0, 0);
    lv_obj_set_style_pad_all(bezel, 0, 0);
    lv_obj_clear_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bezel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(bezel, bcx, 0);
    lv_obj_set_style_transform_pivot_y(bezel, bcy, 0);

    // Rim ring + shadow + highlight ring; all concentric with the
    // dial-center rings drawn on s_root.
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2, 6, theme.panel_edge);
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2 + 10, 2, 0x111a26);  // outer shadow
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2 - 14, 1, 0x0c1828);  // inner highlight

    // Cardinal labels (N/E/S/W large; NE/SE/SW/NW small) - polar from bezel center
    make_label_at_polar_at(bezel, bcx, bcy, "N", 0, R_BEZEL - 22, &lv_font_montserrat_20, theme.fg);
    make_label_at_polar_at(bezel, bcx, bcy, "E", 90, R_BEZEL - 22, &lv_font_montserrat_20,
                           theme.fg);
    make_label_at_polar_at(bezel, bcx, bcy, "S", 180, R_BEZEL - 22, &lv_font_montserrat_20,
                           theme.fg);
    make_label_at_polar_at(bezel, bcx, bcy, "W", 270, R_BEZEL - 22, &lv_font_montserrat_20,
                           theme.fg);
    make_label_at_polar_at(bezel, bcx, bcy, "NE", 45, R_BEZEL - 22, &lv_font_montserrat_14,
                           theme.fg_dim);
    make_label_at_polar_at(bezel, bcx, bcy, "SE", 135, R_BEZEL - 22, &lv_font_montserrat_14,
                           theme.fg_dim);
    make_label_at_polar_at(bezel, bcx, bcy, "SW", 225, R_BEZEL - 22, &lv_font_montserrat_14,
                           theme.fg_dim);
    make_label_at_polar_at(bezel, bcx, bcy, "NW", 315, R_BEZEL - 22, &lv_font_montserrat_14,
                           theme.fg_dim);

    // 22.5deg tick marks (between cardinals + intercardinals)
    for (int deg = 0; deg < 360; deg += 45) {
        make_tick_at(bezel, bcx, bcy, deg + 22, 10, 2, theme.fg_dim);
    }

    // Fixed bow indicator (notch on the rim) — a small white triangle
    // *outside* the bezel group so it stays at top (not part of bezel).
    lv_obj_t *bow_notch = lv_obj_create(parent);
    lv_obj_set_size(bow_notch, 8, 12);
    lv_obj_set_pos(bow_notch, CX - 4, CY - R_BEZEL - 14);
    lv_obj_set_style_bg_color(bow_notch, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(bow_notch, LV_OPA_90, 0);
    lv_obj_set_style_border_width(bow_notch, 0, 0);
    lv_obj_set_style_radius(bow_notch, 2, 0);
    lv_obj_set_style_pad_all(bow_notch, 0, 0);
    lv_obj_clear_flag(bow_notch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bow_notch, LV_OBJ_FLAG_CLICKABLE);
}

// ---- static (bow-relative) overlay -------------------------------------

// Close-hauled bands at ~30° each side of bow. LVGL angles: 0 = right
// (3 o'clock), 270 = up (12 o'clock). For a span from bow-30 to bow on
// the port side, that maps to LVGL angles 240..270. Starboard 270..300.
static void build_close_hauled(lv_obj_t *parent) {
    // Port (red)
    lv_obj_t *port = lv_arc_create(parent);
    lv_obj_remove_style(port, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(port, LV_OBJ_FLAG_CLICKABLE);
    int d = R_CLOSEHAULED * 2;
    lv_obj_set_size(port, d, d);
    lv_obj_set_pos(port, CX - R_CLOSEHAULED, CY - R_CLOSEHAULED);
    lv_arc_set_rotation(port, 0);
    lv_arc_set_bg_angles(port, 240, 270);  // -30° to bow
    lv_arc_set_angles(port, 240, 270);
    lv_obj_set_style_arc_color(port, lv_color_hex(theme.port), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(port, lv_color_hex(theme.port), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(port, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(port, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_width(port, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(port, 6, LV_PART_MAIN);

    // Starboard (green)
    lv_obj_t *stbd = lv_arc_create(parent);
    lv_obj_remove_style(stbd, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(stbd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(stbd, d, d);
    lv_obj_set_pos(stbd, CX - R_CLOSEHAULED, CY - R_CLOSEHAULED);
    lv_arc_set_rotation(stbd, 0);
    lv_arc_set_bg_angles(stbd, 270, 300);  // bow to +30°
    lv_arc_set_angles(stbd, 270, 300);
    lv_obj_set_style_arc_color(stbd, lv_color_hex(theme.starboard), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(stbd, lv_color_hex(theme.starboard), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(stbd, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(stbd, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_width(stbd, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(stbd, 6, LV_PART_MAIN);
}

// Wind-angle-off-bow scale (30, 60, 90, 120, 150) - bow-relative.
static void build_wind_scale(lv_obj_t *parent) {
    static const int angles[] = {30, 60, 90, 120, 150};
    char buf[8];
    for (int i = 0; i < 5; ++i) {
        snprintf(buf, sizeof(buf), "%d", angles[i]);
        make_label_at_polar(parent, buf, -angles[i], R_FACE - 26, &lv_font_montserrat_14,
                            theme.fg_dim);
        make_label_at_polar(parent, buf, angles[i], R_FACE - 26, &lv_font_montserrat_14,
                            theme.fg_dim);
    }
}

// Stylised boat outline: thin vertical centerline + a slim teardrop frame.
static void build_boat(lv_obj_t *parent) {
    // Centerline (port-starboard axis hint)
    lv_obj_t *cl = lv_obj_create(parent);
    lv_obj_set_size(cl, 1, R_FACE);
    lv_obj_set_pos(cl, CX, CY - R_FACE / 2);
    lv_obj_set_style_bg_color(cl, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(cl, LV_OPA_20, 0);
    lv_obj_set_style_border_width(cl, 0, 0);
    lv_obj_set_style_pad_all(cl, 0, 0);
    lv_obj_clear_flag(cl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cl, LV_OBJ_FLAG_CLICKABLE);

    // Boat hull as a polyline. Points scaled from the reference card
    // (SVG center 150,150 -> dial center) and proportioned for radius.
    // Hull half-width ~28, length ~155 (relative to dial center).
    static lv_point_precise_t pts[] = {
        {CX - 28, CY + 63}, {CX - 28, CY - 7}, {CX - 22, CY - 42}, {CX, CY - 92},
        {CX + 22, CY - 42}, {CX + 28, CY - 7}, {CX + 28, CY + 63},
    };
    lv_obj_t *hull = lv_line_create(parent);
    lv_line_set_points(hull, pts, sizeof(pts) / sizeof(pts[0]));
    lv_obj_set_style_line_color(hull, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_line_opa(hull, LV_OPA_30, 0);
    lv_obj_set_style_line_width(hull, 3, 0);
    lv_obj_set_style_line_rounded(hull, true, 0);
    lv_obj_clear_flag(hull, LV_OBJ_FLAG_CLICKABLE);
}

// ---- wind markers (T and A) --------------------------------------------

static lv_obj_t *make_wind_marker(lv_obj_t *parent, const char *letter, uint32_t bg, uint32_t fg) {
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, 28, 32);
    lv_obj_set_style_bg_color(m, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_set_style_radius(m, 6, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_center(m, 14, R_MARKER);

    lv_obj_t *l = lv_label_create(m);
    lv_label_set_text(l, letter);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    return m;
}

// ---- tide arrow --------------------------------------------------------
// A small triangle (head) + rectangle (tail) at dial center, rotating
// around the center. Distinct cool-blue color, hidden when no current.
static void build_tide(lv_obj_t *parent) {
    tide_arrow = lv_obj_create(parent);
    lv_obj_set_size(tide_arrow, 22, 80);
    lv_obj_set_style_bg_opa(tide_arrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tide_arrow, 0, 0);
    lv_obj_set_style_pad_all(tide_arrow, 0, 0);
    lv_obj_clear_flag(tide_arrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tide_arrow, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_center(tide_arrow, 11, 40);

    // Tail
    lv_obj_t *tail = lv_obj_create(tide_arrow);
    lv_obj_set_size(tail, 6, 50);
    lv_obj_set_pos(tail, 8, 22);
    lv_obj_set_style_bg_color(tail, lv_color_hex(0x288cff), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_70, 0);
    lv_obj_set_style_border_width(tail, 0, 0);
    lv_obj_set_style_radius(tail, 2, 0);
    lv_obj_set_style_pad_all(tail, 0, 0);
    lv_obj_clear_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tail, LV_OBJ_FLAG_CLICKABLE);

    // Head (filled diamond, approximated with a wider rounded rect)
    lv_obj_t *head = lv_obj_create(tide_arrow);
    lv_obj_set_size(head, 22, 22);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x288cff), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_radius(head, 11, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(tide_arrow, LV_OBJ_FLAG_HIDDEN);
}

// ---- waypoint pip (small yellow marker at rim) -------------------------
static void build_waypoint(lv_obj_t *parent) {
    waypoint_marker = lv_obj_create(parent);
    lv_obj_set_size(waypoint_marker, 14, 18);
    lv_obj_set_style_bg_color(waypoint_marker, lv_color_hex(0xffd21f), 0);
    lv_obj_set_style_bg_opa(waypoint_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(waypoint_marker, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(waypoint_marker, 1, 0);
    lv_obj_set_style_radius(waypoint_marker, 4, 0);
    lv_obj_set_style_pad_all(waypoint_marker, 0, 0);
    lv_obj_clear_flag(waypoint_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(waypoint_marker, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_center(waypoint_marker, 7, R_BEZEL + 4);
    lv_obj_add_flag(waypoint_marker, LV_OBJ_FLAG_HIDDEN);
}

// ---- corner data boxes -------------------------------------------------

static void make_data_box(lv_obj_t *parent, const char *label, const char *unit, int x, int y,
                          int w, int h, lv_obj_t **out_value, uint32_t value_color,
                          uint32_t accent_color) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rail = lv_obj_create(box);
    lv_obj_set_size(rail, 3, h - 12);
    lv_obj_set_pos(rail, 0, 6);
    lv_obj_set_style_bg_color(rail, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 2, 0);
    lv_obj_set_style_pad_all(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_label_create(box);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(cap, 10, 3);

    lv_obj_t *unit_lbl = lv_label_create(box);
    lv_label_set_text(unit_lbl, unit);
    lv_obj_set_style_text_font(unit_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit_lbl, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(unit_lbl, LV_ALIGN_TOP_RIGHT, 0, 3);

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "-");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(value_color), 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 10, 0);
    *out_value = val;
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

    // Layer order: face -> close-hauled arcs -> wind-angle scale -> boat
    // -> tide -> wind markers -> bezel -> waypoint pip -> overlays
    make_ring(s_root, R_FACE * 2, 0, theme.panel);  // face background
    lv_obj_t *face = lv_obj_create(s_root);
    lv_obj_set_size(face, R_FACE * 2, R_FACE * 2);
    lv_obj_set_pos(face, CX - R_FACE, CY - R_FACE);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_opa(face, LV_OPA_60, 0);
    lv_obj_set_style_border_width(face, 1, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);

    build_close_hauled(s_root);
    build_wind_scale(s_root);
    build_boat(s_root);
    build_tide(s_root);

    // Tide speed text at dial center (under markers, over boat)
    lbl_tide_speed = lv_label_create(s_root);
    lv_label_set_text(lbl_tide_speed, "");
    lv_obj_set_style_text_font(lbl_tide_speed, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_tide_speed, lv_color_hex(theme.fg), 0);
    lv_obj_set_pos(lbl_tide_speed, CX - 22, CY - 10);

    // Wind markers (T white, A amber). T below A in z-order so amber wins
    // when they overlap (apparent is the one you steer to).
    twa_marker = make_wind_marker(s_root, "T", 0xe3ebf2, 0x13253a);
    awa_marker = make_wind_marker(s_root, "A", 0xf6a21a, 0x13253a);

    build_bezel(s_root);
    build_waypoint(s_root);

    // ---- four corner data boxes + bow/stern strip ---------------------
    // Top-left: AWS. Top-right would conflict with the global MOB pill
    // (LV_ALIGN_TOP_RIGHT -6, 6, 56x56, on lv_layer_top) so TWS is
    // shifted down below the safe zone. Keep visual pairing by also
    // pushing AWS slightly so they share a baseline.
    // Corner boxes sized to 88×54 so their inner corners clear the bezel ring.
    // AWA/TWA moved from y=211 (right on the 90° scale text) to y=265
    // (below the 90° zone at y≈230-250, above the 120° zone at y≈322).
    make_data_box(s_root, "AWS", "kn", 12, 10, 88, 54, &lbl_aws_value, theme.fg, 0xf6a21a);
    make_data_box(s_root, "TWS", "kn", LCD_W - 100, 70, 88, 54, &lbl_tws_value, theme.fg,
                  theme.fg_dim);
    // Mid-left: AWA  /  Mid-right: TWA  (below the 90° wind-scale labels)
    make_data_box(s_root, "AWA", "deg", 12, 265, 88, 54, &lbl_awa_value, 0xf6a21a, 0xf6a21a);
    make_data_box(s_root, "TWA", "deg", LCD_W - 100, 265, 88, 54, &lbl_twa_value, theme.fg,
                  theme.fg_dim);
    // Bottom-left: HDG  /  Bottom-right: COG
    make_data_box(s_root, "HDG", "T", 12, LCD_H - 68, 88, 54, &lbl_hdg_value, theme.accent,
                  theme.accent);
    make_data_box(s_root, "COG", "T", LCD_W - 100, LCD_H - 68, 88, 54, &lbl_cog_value, theme.fg,
                  theme.fg_dim);

    return s_root;
}

// ---- refresh -----------------------------------------------------------

static int16_t deg_to_lvgl(double deg) {
    // LVGL transform_rotation is in 0.1° units, [0..3600). Quantize to
    // whole degrees - sub-degree sensor jitter would otherwise force a
    // re-invalidate of large transformed bounding boxes on every refresh
    // and stall the SW renderer.
    int16_t r = (int16_t)(lround(deg) * 10);
    while (r < 0)
        r += 3600;
    while (r >= 3600)
        r -= 3600;
    return r;
}

static volatile bool s_refresh_enabled = true;
void set_refresh_enabled(bool e) {
    s_refresh_enabled = e;
}
bool refresh_enabled() {
    return s_refresh_enabled;
}

// Dirty-value caches per docs/specs/09 "Implementation Improvements"
// #1. Setters are skipped when the displayed value would not change.
// Cached values use a single sentinel (INT16_MIN for rotations, "\0xff"
// in [0] for unset text) so the first refresh always writes.
static char s_last_aws[16] = {(char)0xFF};
static char s_last_tws[16] = {(char)0xFF};
static char s_last_awa[16] = {(char)0xFF};
static char s_last_twa[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_cog[16] = {(char)0xFF};
static char s_last_tide[16] = {(char)0xFF};
static int16_t s_last_awa_rot = INT16_MIN;
static int16_t s_last_twa_rot = INT16_MIN;
static int16_t s_last_bezel_rot = INT16_MIN;
static int16_t s_last_tide_rot = INT16_MIN;
static int16_t s_last_wp_rot = INT16_MIN;
static int8_t s_last_awa_hidden = -1;  // -1 = unset, 0 = shown, 1 = hidden
static int8_t s_last_twa_hidden = -1;
static int8_t s_last_tide_hidden = -1;
static int8_t s_last_wp_hidden = -1;

// Helpers moved to include/ui_dirty.h - shared across all screens.
using ui::set_hidden_if_changed;
using ui::set_rot_if_changed;
using ui::set_text_if_changed;

void refresh() {
    if (!s_refresh_enabled) return;
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    // --- hero speed readouts ---
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        set_text_if_changed(lbl_aws_value, s_last_aws, sizeof(s_last_aws), buf);
    } else {
        set_text_if_changed(lbl_aws_value, s_last_aws, sizeof(s_last_aws), "--");
    }
    if (!isnan(d.tws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.tws));
        set_text_if_changed(lbl_tws_value, s_last_tws, sizeof(s_last_tws), buf);
    } else {
        set_text_if_changed(lbl_tws_value, s_last_tws, sizeof(s_last_tws), "--");
    }

    // --- AWA: angle text in port/stbd form + marker rotation ---
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        set_text_if_changed(lbl_awa_value, s_last_awa, sizeof(s_last_awa), buf);
        set_rot_if_changed(awa_marker, &s_last_awa_rot, deg_to_lvgl(deg));
        set_hidden_if_changed(awa_marker, &s_last_awa_hidden, false);
    } else {
        // No live data: hide the marker entirely. The earlier "sweep
        // slowly so the screen reads alive" animation forced a wind-
        // bezel-sized invalidation every refresh and was a major hit
        // on RenderLatency / FrameInterval (see docs/specs/09).
        set_text_if_changed(lbl_awa_value, s_last_awa, sizeof(s_last_awa), "--");
        set_hidden_if_changed(awa_marker, &s_last_awa_hidden, true);
    }

    // --- TWA ---
    if (!isnan(d.twa)) {
        double deg = rad_to_deg_pos(d.twa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        set_text_if_changed(lbl_twa_value, s_last_twa, sizeof(s_last_twa), buf);
        set_rot_if_changed(twa_marker, &s_last_twa_rot, deg_to_lvgl(deg));
        set_hidden_if_changed(twa_marker, &s_last_twa_hidden, false);
    } else {
        set_text_if_changed(lbl_twa_value, s_last_twa, sizeof(s_last_twa), "--");
        set_hidden_if_changed(twa_marker, &s_last_twa_hidden, true);
    }

    // --- bezel rotation (heading) ---
    double hdg_deg = NAN;
    if (!isnan(d.headingTrue)) hdg_deg = rad_to_deg_pos(d.headingTrue);
    if (!isnan(hdg_deg)) {
        set_rot_if_changed(bezel, &s_last_bezel_rot, deg_to_lvgl(-hdg_deg));
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", hdg_deg);
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), "--\xC2\xB0");
    }

    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        set_text_if_changed(lbl_cog_value, s_last_cog, sizeof(s_last_cog), buf);
    } else {
        set_text_if_changed(lbl_cog_value, s_last_cog, sizeof(s_last_cog), "--\xC2\xB0");
    }

    // --- tide arrow ---
    bool show_tide = !isnan(d.currentSetTrue) && !isnan(d.currentDrift) && d.currentDrift > 0.05 &&
                     !isnan(hdg_deg);
    if (show_tide) {
        double tide_rel = rad_to_deg_pos(d.currentSetTrue) - hdg_deg;
        while (tide_rel < 0)
            tide_rel += 360;
        set_rot_if_changed(tide_arrow, &s_last_tide_rot, deg_to_lvgl(tide_rel));
        set_hidden_if_changed(tide_arrow, &s_last_tide_hidden, false);
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.currentDrift));
        set_text_if_changed(lbl_tide_speed, s_last_tide, sizeof(s_last_tide), buf);
    } else {
        set_hidden_if_changed(tide_arrow, &s_last_tide_hidden, true);
        set_text_if_changed(lbl_tide_speed, s_last_tide, sizeof(s_last_tide), "");
    }

    // --- waypoint pip ---
    if (!isnan(d.btw) && !isnan(hdg_deg)) {
        double wp_rel = rad_to_deg_pos(d.btw) - hdg_deg;
        while (wp_rel < 0)
            wp_rel += 360;
        set_rot_if_changed(waypoint_marker, &s_last_wp_rot, deg_to_lvgl(wp_rel));
        set_hidden_if_changed(waypoint_marker, &s_last_wp_hidden, false);
    } else {
        set_hidden_if_changed(waypoint_marker, &s_last_wp_hidden, true);
    }
}

}  // namespace ui::wind
