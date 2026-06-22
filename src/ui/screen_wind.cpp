#include "screens.h"
#include "ui_theme.h"
#include "ui_compass.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen wind dial, reference glass-cockpit language. The 360° rose is
// retained (wind can blow from any bearing, including dead astern, so a half
// gauge would be ambiguous) but ALL numerics live in rounded tiles outside the
// rose — the previous in-dial AWS/TWS/HDG hero readouts and the 30…150 angle
// scale collided with the rotating bezel cardinals and the A/T markers, which
// was the visible label-overlap rendering defect. Now the rose carries only
// graphical indices; the numbers sit in clean tiles below (square) or in side
// columns (wide), mirroring the autopilot HUD.
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
static lv_obj_t *tide_zero = nullptr;  // circle shown when current ~ 0
static lv_obj_t *waypoint_marker = nullptr;

// Numeric tiles (outside the rose) + the compact HDG/drift labels at centre.
static lv_obj_t *tile_aws, *tile_awa, *tile_tws, *tile_twa;
static lv_obj_t *lbl_hdg_value, *lbl_tide_speed, *lbl_drift_cap;

// Geometry derived from LCD_W/LCD_H so the same source compiles for 480x480
// sunton, 800x480 waveshare-4.3/5/7, and 1024x600 waveshare-5/7B. Square panels
// reserve a bottom tile band; wide panels flank the rose with side columns.
static constexpr bool WIDE = (LCD_W * 100 > LCD_H * 125);
static constexpr int TILE_BAND = WIDE ? 0 : 132;  // square: bottom tile row
static constexpr int ROSE_AREA_W = WIDE ? (LCD_H - 8) : LCD_W;
static constexpr int ROSE_AREA_H = LCD_H - TILE_BAND;
static constexpr int CX = LCD_W / 2;
static constexpr int CY = ROSE_AREA_H / 2;
static constexpr int DIAL_SHORTSIDE = (ROSE_AREA_W < ROSE_AREA_H ? ROSE_AREA_W : ROSE_AREA_H);
static constexpr int R_BEZEL = DIAL_SHORTSIDE / 2 - 16;
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

// Upright cardinal / intercardinal labels for the rotating rose. Unlike the
// ticks (which rotate with the bezel), these stay HORIZONTAL and are
// repositioned around the white band per heading by layout_cardinals(), so they
// are always readable and high-contrast (dark ink on the white band) instead of
// tilting or going upside-down.
static lv_obj_t *card_lbl[8];
static const int kCardBearing[8] = {0, 45, 90, 135, 180, 225, 270, 315};
static const char *kCardText[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

static void build_cardinals(lv_obj_t *parent) {
    for (int i = 0; i < 8; ++i) {
        bool card = (i % 2) == 0;  // N / E / S / W
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, kCardText[i]);
        lv_obj_set_style_text_font(l, card ? &lv_font_montserrat_20 : &lv_font_montserrat_14, 0);
        // Neutral ink for all cardinals (N no longer red): red is reserved for
        // port-side cues on the wind dial, so the N letter must not claim it.
        uint32_t color = card ? 0x16222f : 0x44546a;
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_set_width(l, 40);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        card_lbl[i] = l;
    }
}

// Reposition the upright cardinals on the white band for the current heading
// (screen angle = bearing - heading, 0 = up). Called from refresh on change.
static void layout_cardinals(double hdg_deg) {
    // The 26 px white band's border draws inward from R_BEZEL, so its centre is
    // ~13 px in; place the labels there to sit ON the white (high contrast).
    int R = R_BEZEL - 13;
    for (int i = 0; i < 8; ++i) {
        if (!card_lbl[i]) continue;
        double a = (kCardBearing[i] - hdg_deg) * M_PI / 180.0;
        int x = CX + (int)(R * sin(a));
        int y = CY - (int)(R * cos(a));
        int hh = ((i % 2) == 0) ? 13 : 10;
        lv_obj_set_pos(card_lbl[i], x - 20, y - hh);
        lv_obj_clear_flag(card_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
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

    // Wide white band + green rail (reference rim), then the structural rings.
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2, 26, theme.arc_band, LV_OPA_90);
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2 + 18, 7, theme.good, LV_OPA_80);
    make_ring_at(bezel, bcx, bcy, R_BEZEL * 2 - 26, 1, 0x0c1828);  // inner highlight

    // Cardinal labels are NOT children of the bezel — they live in a separate
    // upright overlay (see build_cardinals / layout_cardinals) so they stay
    // horizontal and high-contrast instead of tilting with the rotating rim.

    // 22.5deg tick marks (between cardinals + intercardinals)
    for (int deg = 0; deg < 360; deg += 45) {
        make_tick_at(bezel, bcx, bcy, deg + 22, 10, 2, 0x5a6b78);
    }

    // Fixed bow indicator (notch on the rim) — a small red triangle *outside*
    // the bezel group so it stays at the top (the boat's heading reference).
    lv_obj_t *bow_notch = lv_label_create(parent);
    lv_label_set_text(bow_notch, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(bow_notch, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bow_notch, lv_color_hex(theme.alarm), 0);
    lv_obj_set_pos(bow_notch, CX - 9, CY - R_BEZEL - 6);
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

    // Boat hull as a polyline. Compact (≈60% of the face) so the interior stays
    // open for the HDG readout, wind markers, and the tidal current vector.
    int hw = R_FACE * 20 / 130;  // hull half-width
    int bow = R_FACE * 56 / 130;
    int mid = R_FACE * 26 / 130;
    int shoulder = R_FACE * 4 / 130;
    int stern = R_FACE * 40 / 130;
    static lv_point_precise_t pts[7];
    pts[0] = {CX - hw, CY + stern};
    pts[1] = {CX - hw, CY - shoulder};
    pts[2] = {CX - hw * 22 / 28, CY - mid};
    pts[3] = {CX, CY - bow};
    pts[4] = {CX + hw * 22 / 28, CY - mid};
    pts[5] = {CX + hw, CY - shoulder};
    pts[6] = {CX + hw, CY + stern};
    lv_obj_t *hull = lv_line_create(parent);
    lv_line_set_points(hull, pts, sizeof(pts) / sizeof(pts[0]));
    lv_obj_set_style_line_color(hull, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_line_opa(hull, LV_OPA_30, 0);
    lv_obj_set_style_line_width(hull, 3, 0);
    lv_obj_set_style_line_rounded(hull, true, 0);
    lv_obj_clear_flag(hull, LV_OBJ_FLAG_CLICKABLE);
}

// ---- wind markers (T and A) --------------------------------------------

// A wind index that orbits the rim at the wind-source bearing: a filled
// triangle (LV_SYMBOL_DOWN) pointing INWARD — i.e. in the direction the wind is
// blowing across the boat — with the A / T letter riding just inboard of it.
// The holder pivots at the dial centre, so set_rot_if_changed() in refresh()
// sweeps the whole index around the rose to AWA / TWA.
static lv_obj_t *make_wind_marker(lv_obj_t *parent, const char *letter, uint32_t color) {
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, 34, 76);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_center(m, 17, R_MARKER);  // pivot at dial centre; top edge near rim

    // Filled triangle head at the rim, pointing inward toward the boat.
    lv_obj_t *tri = lv_label_create(m);
    lv_label_set_text(tri, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(tri, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(tri, lv_color_hex(color), 0);
    lv_obj_align(tri, LV_ALIGN_TOP_MID, 0, -6);

    // Long, bold radial stem so the index reads as a clear high-contrast
    // pointer (the previous short triangle alone was too faint).
    lv_obj_t *stem = lv_obj_create(m);
    lv_obj_set_size(stem, 6, 34);
    lv_obj_set_style_bg_color(stem, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stem, 0, 0);
    lv_obj_set_style_radius(stem, 2, 0);
    lv_obj_set_style_pad_all(stem, 0, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 20);

    // A / T letter inboard of the stem, in the marker's high-contrast color.
    lv_obj_t *l = lv_label_create(m);
    lv_label_set_text(l, letter);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 54);
    return m;
}

// ---- tide / current vector ---------------------------------------------
// A current arrow rooted at the dial centre and pointing the way the current is
// flowing TOWARD (its set). When the current is ~0 (calm), the arrow hides and a
// small ring is shown at the centre instead.
static constexpr int TIDE_H = 132;           // box height
static constexpr int TIDE_MID = TIDE_H / 2;  // pivot row == dial centre
static void build_tide(lv_obj_t *parent) {
    tide_arrow = lv_obj_create(parent);
    lv_obj_set_size(tide_arrow, 32, TIDE_H);
    lv_obj_set_style_bg_opa(tide_arrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tide_arrow, 0, 0);
    lv_obj_set_style_pad_all(tide_arrow, 0, 0);
    lv_obj_clear_flag(tide_arrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tide_arrow, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_center(tide_arrow, 16, TIDE_MID);

    // Shaft: from the dial centre (TIDE_MID, the pivot row) outward toward the
    // set end (top of the box), so the arrow emanates from the boat.
    lv_obj_t *shaft = lv_obj_create(tide_arrow);
    lv_obj_set_size(shaft, 5, 54);
    lv_obj_set_pos(shaft, 16 - 2, TIDE_MID - 54);
    lv_obj_set_style_bg_color(shaft, lv_color_hex(0x288cff), 0);
    lv_obj_set_style_bg_opa(shaft, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shaft, 0, 0);
    lv_obj_set_style_radius(shaft, 2, 0);
    lv_obj_set_style_pad_all(shaft, 0, 0);
    lv_obj_clear_flag(shaft, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(shaft, LV_OBJ_FLAG_CLICKABLE);

    // Head: filled triangle at the set end, pointing OUTWARD (toward set).
    lv_obj_t *head = lv_label_create(tide_arrow);
    lv_label_set_text(head, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(0x288cff), 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, -4);
    lv_obj_add_flag(tide_arrow, LV_OBJ_FLAG_HIDDEN);

    // Zero-current ring (shown when drift ~ 0): a hollow circle at the centre.
    tide_zero = lv_obj_create(parent);
    lv_obj_set_size(tide_zero, 26, 26);
    apply_pivot_center(tide_zero, 13, 13);  // centred on the dial
    lv_obj_set_style_bg_opa(tide_zero, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(tide_zero, lv_color_hex(0x288cff), 0);
    lv_obj_set_style_border_width(tide_zero, 3, 0);
    lv_obj_set_style_radius(tide_zero, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(tide_zero, 0, 0);
    lv_obj_clear_flag(tide_zero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tide_zero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tide_zero, LV_OBJ_FLAG_HIDDEN);
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

// ---- tiles -------------------------------------------------------------

static void build_tiles(lv_obj_t *parent) {
    const lv_font_t *tv = &lv_font_montserrat_38;
    if (!WIDE) {
        int gap = 8, n = 4;
        int tw = (LCD_W - gap * (n + 1)) / n;
        int th = TILE_BAND - 16;
        int ty = LCD_H - th - 8;
        tile_aws = ui::numeric_tile(parent, gap, ty, tw, th, "AWS", "kn", tv, theme.warn);
        tile_awa = ui::numeric_tile(parent, gap * 2 + tw, ty, tw, th, "AWA", "", tv, theme.fg);
        tile_tws =
            ui::numeric_tile(parent, gap * 3 + tw * 2, ty, tw, th, "TWS", "kn", tv, theme.fg);
        tile_twa = ui::numeric_tile(parent, gap * 4 + tw * 3, ty, tw, th, "TWA", "", tv, theme.fg);
    } else {
        int gap = 8;
        int rose_left = CX - ROSE_AREA_W / 2;
        int rose_right = CX + ROSE_AREA_W / 2;
        int colw_l = rose_left - gap * 2;
        int colw_r = (LCD_W - rose_right) - gap * 2;
        int th = (LCD_H - gap * 3) / 2;
        int ty = gap;
        tile_aws = ui::numeric_tile(parent, gap, ty, colw_l, th, "AWS", "kn", tv, theme.warn);
        tile_awa =
            ui::numeric_tile(parent, gap, ty + th + gap, colw_l, th, "AWA", "", tv, theme.warn);
        tile_tws =
            ui::numeric_tile(parent, rose_right + gap, ty, colw_r, th, "TWS", "kn", tv, theme.fg);
        tile_twa = ui::numeric_tile(parent, rose_right + gap, ty + th + gap, colw_r, th, "TWA", "",
                                    tv, theme.fg);
    }
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

    // Layer order: face -> close-hauled arcs -> boat -> tide -> wind markers
    // -> bezel -> waypoint pip -> centre labels -> tiles.
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
    build_boat(s_root);
    build_tide(s_root);

    // Tide/current drift speed at dial center (under markers, over boat). The
    // bare number used to read as an unlabeled "0.8" near the boat icon; a small
    // "DRIFT" caption + "kn" unit make it unambiguous (this is current set/drift
    // speed, bound to sk::Data.currentDrift, paired with the blue set arrow).
    lbl_drift_cap = lv_label_create(s_root);
    lv_label_set_text(lbl_drift_cap, "DRIFT");
    lv_obj_set_style_text_font(lbl_drift_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_drift_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(lbl_drift_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_drift_cap, 60);
    lv_obj_set_pos(lbl_drift_cap, CX - 30, CY + 8);
    lv_obj_add_flag(lbl_drift_cap, LV_OBJ_FLAG_HIDDEN);

    lbl_tide_speed = lv_label_create(s_root);
    lv_label_set_text(lbl_tide_speed, "");
    lv_obj_set_style_text_font(lbl_tide_speed, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_tide_speed, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(lbl_tide_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_tide_speed, 60);
    lv_obj_set_pos(lbl_tide_speed, CX - 30, CY + 24);

    // Wind indices (T white, A amber). T below A in z-order so amber wins when
    // they overlap (apparent is the one you steer to).
    // High-contrast, clearly distinct colors: A (apparent, the one you steer to)
    // in orange, T (true) in cyan. Both read on the white band and dark face.
    twa_marker = make_wind_marker(s_root, "T", 0x2bd4e8);
    awa_marker = make_wind_marker(s_root, "A", 0xff8800);

    build_bezel(s_root);
    build_cardinals(s_root);  // upright cardinal overlay (laid out per heading)
    build_waypoint(s_root);

    // HDG readout in the upper interior, just below the bezel and clear of the
    // (compact) boat — the bezel rim + red bow notch show the track graphically,
    // this gives the digits. Positioned near the top so it never sits on the
    // hull or the central current vector.
    lv_obj_t *hcap = lv_label_create(s_root);
    lv_label_set_text(hcap, "HDG");
    lv_obj_set_style_text_font(hcap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hcap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(hcap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hcap, 80);
    lv_obj_set_pos(hcap, CX - 40, CY - R_FACE + 30);

    lbl_hdg_value = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg_value, "--\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg_value, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_hdg_value, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_text_align(lbl_hdg_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_hdg_value, 120);
    lv_obj_set_pos(lbl_hdg_value, CX - 60, CY - R_FACE + 50);

    build_tiles(s_root);

    return s_root;
}

// ---- refresh -----------------------------------------------------------

using ui::deg_to_lvgl;  // shared (include/ui_dirty.h)

static volatile bool s_refresh_enabled = true;
void set_refresh_enabled(bool e) {
    s_refresh_enabled = e;
}
bool refresh_enabled() {
    return s_refresh_enabled;
}

// Dirty-value caches: setters are skipped when the displayed value would not
// change so LVGL's partial render has nothing to invalidate.
static char s_last_aws[16] = {(char)0xFF};
static char s_last_tws[16] = {(char)0xFF};
static char s_last_awa[16] = {(char)0xFF};
static char s_last_twa[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_tide[16] = {(char)0xFF};
static int16_t s_last_awa_rot = INT16_MIN;
static int16_t s_last_twa_rot = INT16_MIN;
static int16_t s_last_bezel_rot = INT16_MIN;
static int16_t s_last_tide_rot = INT16_MIN;
static int16_t s_last_wp_rot = INT16_MIN;
static int8_t s_last_awa_hidden = -1;  // -1 = unset, 0 = shown, 1 = hidden
static int8_t s_last_twa_hidden = -1;
static int8_t s_last_tide_hidden = -1;
static int8_t s_last_tide_zero_hidden = -1;
static int8_t s_last_wp_hidden = -1;
static int8_t s_last_drift_cap_hidden = -1;

using ui::set_hidden_if_changed;
using ui::set_rot_if_changed;
using ui::set_text_if_changed;

void refresh() {
    if (!s_refresh_enabled) return;
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    // --- speed tiles ---
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), buf);
    } else {
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), "--");
    }
    if (!isnan(d.tws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.tws));
        set_text_if_changed(tile_tws, s_last_tws, sizeof(s_last_tws), buf);
    } else {
        set_text_if_changed(tile_tws, s_last_tws, sizeof(s_last_tws), "--");
    }

    // --- AWA: angle tile (port/stbd form) + marker rotation ---
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), buf);
        set_rot_if_changed(awa_marker, &s_last_awa_rot, deg_to_lvgl(deg));
        set_hidden_if_changed(awa_marker, &s_last_awa_hidden, false);
    } else {
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), "--");
        set_hidden_if_changed(awa_marker, &s_last_awa_hidden, true);
    }

    // --- TWA ---
    if (!isnan(d.twa)) {
        double deg = rad_to_deg_pos(d.twa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        set_text_if_changed(tile_twa, s_last_twa, sizeof(s_last_twa), buf);
        set_rot_if_changed(twa_marker, &s_last_twa_rot, deg_to_lvgl(deg));
        set_hidden_if_changed(twa_marker, &s_last_twa_hidden, false);
    } else {
        set_text_if_changed(tile_twa, s_last_twa, sizeof(s_last_twa), "--");
        set_hidden_if_changed(twa_marker, &s_last_twa_hidden, true);
    }

    // --- bezel rotation (heading) + upright cardinal layout + HDG digits ---
    double hdg_deg = NAN;
    if (!isnan(d.headingTrue)) hdg_deg = rad_to_deg_pos(d.headingTrue);
    double card_hdg = isnan(hdg_deg) ? 0.0 : hdg_deg;  // north-up when no heading
    int16_t bez_rot = deg_to_lvgl(-card_hdg);
    if (bez_rot != s_last_bezel_rot) {
        s_last_bezel_rot = bez_rot;
        lv_obj_set_style_transform_rotation(bezel, bez_rot, 0);
        layout_cardinals(card_hdg);
    }
    if (!isnan(hdg_deg)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", hdg_deg);
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), "--\xC2\xB0");
    }

    // --- tide / current vector ---
    bool have_current = !isnan(d.currentSetTrue) && !isnan(d.currentDrift) && !isnan(hdg_deg);
    bool flowing = have_current && d.currentDrift > 0.05;
    if (flowing) {
        double tide_rel = rad_to_deg_pos(d.currentSetTrue) - hdg_deg;
        while (tide_rel < 0)
            tide_rel += 360;
        set_rot_if_changed(tide_arrow, &s_last_tide_rot, deg_to_lvgl(tide_rel));
        set_hidden_if_changed(tide_arrow, &s_last_tide_hidden, false);
        set_hidden_if_changed(tide_zero, &s_last_tide_zero_hidden, true);
        snprintf(buf, sizeof(buf), "%.1fkn", mps_to_kn(d.currentDrift));
        set_text_if_changed(lbl_tide_speed, s_last_tide, sizeof(s_last_tide), buf);
        set_hidden_if_changed(lbl_drift_cap, &s_last_drift_cap_hidden, false);
    } else {
        set_hidden_if_changed(tide_arrow, &s_last_tide_hidden, true);
        set_hidden_if_changed(tide_zero, &s_last_tide_zero_hidden, !have_current);
        set_text_if_changed(lbl_tide_speed, s_last_tide, sizeof(s_last_tide),
                            have_current ? "0.0kn" : "");
        // Show the DRIFT caption only when there is current data to label.
        set_hidden_if_changed(lbl_drift_cap, &s_last_drift_cap_hidden, !have_current);
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
