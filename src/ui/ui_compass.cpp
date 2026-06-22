#include "ui_compass.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

// Reference glass-cockpit primitives: a semicircular heading compass, rounded
// numeric tiles, and a cross-track-error strip. See include/ui_compass.h for the
// rotation contract. Geometry is parametric so the same code serves 480x480,
// 800x480, and 1024x600.

namespace ui {

// ---- small local helpers ----------------------------------------------------

static void no_chrome(lv_obj_t *o) {
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

// A static colored band drawn as an lv_arc background (no knob/indicator
// interaction). `a0`/`a1` are LVGL angles (0 = 3 o'clock, 90 = 6 o'clock).
static lv_obj_t *band_arc(lv_obj_t *parent, int cx, int cy, int radius, int width, uint32_t color,
                          int a0, int a1, lv_opa_t opa) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    int d = radius * 2;
    lv_obj_set_size(arc, d, d);
    lv_obj_set_pos(arc, cx - radius, cy - radius);
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

// A thin rectangle sticking inward from `rim_radius`, pivoting around the
// group's center so the parent scale's rotation sweeps it around the dial.
static void radial_tick(lv_obj_t *group, int gcx, int gcy, int deg, int len, int width,
                        int rim_radius, uint32_t color) {
    lv_obj_t *t = lv_obj_create(group);
    lv_obj_set_size(t, width, len);
    lv_obj_set_style_bg_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    no_chrome(t);
    lv_obj_set_style_radius(t, 1, 0);
    lv_obj_set_pos(t, gcx - width / 2, gcy - rim_radius);
    lv_obj_set_style_transform_pivot_x(t, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(t, rim_radius, 0);
    lv_obj_set_style_transform_rotation(t, deg * 10, 0);
}

// ---- compass ----------------------------------------------------------------

// Where the upright degree labels sit, as an inward offset from the rim radius.
// Pushed well inside the rim so the (bigger) labels clear the tick marks. Kept
// in one place so build + layout agree.
static constexpr int LABEL_INSET = 44;

// Cardinal labels are dark ink (neutral) like the numeric degree labels -- red
// is reserved for port-side / alarm cues, so the N/E/S/W letters must not steal
// it. They read against the white band on weight (montserrat_28) instead.
static bool is_cardinal(int i) {
    return (i % 3) == 0;  // 0=N 90=E 180=S 270=W at i = 0,3,6,9
}

Compass build_compass(lv_obj_t *parent, int ox, int oy, int w) {
    Compass cp = {};
    int r = w / 2 - 8;
    int cx = w / 2;
    int cy = r;  // flat (diameter) edge sits at y = r
    cp.cx = cx;
    cp.cy = cy;
    cp.r = r;

    // Clip container: only the top semicircle (+ a sliver for the W/E labels at
    // the diameter) is shown; the rotating ring's lower half is clipped away.
    cp.h = r + 24;  // top semicircle + clearance for the bigger diameter labels
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, w, cp.h);
    lv_obj_set_pos(root, ox, oy);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    no_chrome(root);
    lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    cp.root = root;

    // Extra-wide white band with a green outer rail (reference proportions).
    band_arc(root, cx, cy, r, 10, theme.good, 180, 360, LV_OPA_COVER);
    band_arc(root, cx, cy, r - 10, 52, theme.arc_band, 180, 360, LV_OPA_COVER);

    // Rotating scale: ONLY the ticks rotate (the degree labels stay upright and
    // are repositioned by compass_layout_labels so they never tilt or collide).
    // Ticks are dark ink so they read against the white band.
    lv_obj_t *scale = lv_obj_create(root);
    lv_obj_set_size(scale, r * 2, r * 2);
    lv_obj_set_pos(scale, cx - r, cy - r);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    no_chrome(scale);
    lv_obj_set_style_transform_pivot_x(scale, r, 0);
    lv_obj_set_style_transform_pivot_y(scale, r, 0);
    cp.scale = scale;

    int rim = r - 10;
    for (int deg = 0; deg < 360; deg += 10) {
        bool major = (deg % 30) == 0;
        radial_tick(scale, r, r, deg, major ? 12 : 7, major ? 3 : 2, rim,
                    major ? 0x16222f : 0x5a6b78);
    }

    // Upright degree labels (children of root, NOT the rotating group). Created
    // here; positioned in compass_layout_labels() once the heading is known.
    char buf[8];
    for (int i = 0; i < 12; ++i) {
        int deg = i * 30;
        const char *txt;
        if (deg == 0)
            txt = "N";
        else if (deg == 90)
            txt = "E";
        else if (deg == 180)
            txt = "S";
        else if (deg == 270)
            txt = "W";
        else {
            snprintf(buf, sizeof(buf), "%d", deg);
            txt = buf;
        }
        lv_obj_t *l = lv_label_create(root);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(
            l, is_cardinal(i) ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x16222f), 0);
        lv_obj_set_width(l, 52);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        cp.nums[i] = l;
    }

    // The HDG/COG/CTS/target markers are an external ui::MarkerRing the screen
    // builds on its own root (so glyphs orbiting just outside the rim are not
    // clipped by this dial-sized root). Only the fixed lubber lives here.

    // Fixed red lubber at the very top: the boat's heading reference.
    lv_obj_t *lub = lv_label_create(root);
    lv_label_set_text(lub, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(lub, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lub, lv_color_hex(theme.alarm), 0);
    lv_obj_align(lub, LV_ALIGN_TOP_MID, 0, -2);
    cp.lubber = lub;

    return cp;
}

// Reposition the upright degree labels for the current heading so the boat's
// head sits at top. Each label is placed on the white band at screen angle
// (deg - heading); labels that would fall in the clipped lower half are hidden.
// Call from the screen refresh whenever the heading changes.
void compass_layout_labels(const Compass &cp, double hdg_deg) {
    int R = cp.r - LABEL_INSET;
    for (int i = 0; i < 12; ++i) {
        if (!cp.nums[i]) continue;
        double rel = i * 30.0 - hdg_deg;
        while (rel > 180.0)
            rel -= 360.0;
        while (rel < -180.0)
            rel += 360.0;
        if (rel > 96.0 || rel < -96.0) {
            lv_obj_add_flag(cp.nums[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        double a = rel * M_PI / 180.0;
        int x = cp.cx + (int)(R * sin(a));
        int y = cp.cy - (int)(R * cos(a));
        int hh = is_cardinal(i) ? 18 : 13;
        lv_obj_set_pos(cp.nums[i], x - 26, y - hh);
        lv_obj_clear_flag(cp.nums[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- numeric tile -----------------------------------------------------------

lv_obj_t *numeric_tile(lv_obj_t *parent, int x, int y, int w, int h, const char *caption,
                       const char *unit, const lv_font_t *value_font, uint32_t value_color) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    style_panel(box);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_label_create(box);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    if (unit && unit[0]) {
        lv_obj_t *u = lv_label_create(box);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(u, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(u, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, value_font, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(value_color), 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    return val;
}

// ---- XTE strip --------------------------------------------------------------

// Sane upper bound on a displayable cross-track error. The sim has been seen
// publishing degenerate XTE of hundreds of km (no active route / bad reference),
// which renders as a meaningless over-precise number. Beyond 5 nm off track the
// magnitude carries no steering value, so clamp to a ">5 nm" indicator with the
// side preserved. 5 nm = 9260 m.
static constexpr double kXteSaneCapM = 9260.0;  // 5 nm
static constexpr double kMetersPerNm = 1852.0;  // exact

void format_xte(double xte_m, char *out, size_t cap) {
    if (isnan(xte_m)) {
        snprintf(out, cap, "--");
        return;
    }
    char side = xte_m >= 0 ? 'P' : 'S';  // +ve = right of track -> steer port
    if (fabs(xte_m) > kXteSaneCapM) {
        snprintf(out, cap, ">5 nm %c", side);
        return;
    }
    // In-range (<= 5 nm): cross-track in nautical miles + the P/S side suffix.
    // Steering wants nm (the unit the chart plotter / autopilot speak), not raw
    // meters. Two decimals resolves down to ~0.01 nm (~19 m) which is plenty for
    // close-quarters steering, and stays narrow enough for the strip's font.
    snprintf(out, cap, "%.2f nm %c", fabs(xte_m) / kMetersPerNm, side);
}

XteStrip build_centerzero_strip(lv_obj_t *parent, int x, int y, int w, int h,
                                const char *left_label, const char *right_label, double full_scale,
                                int tick_decimals, uint32_t needle_color) {
    XteStrip xs = {};
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    no_chrome(root);
    xs.root = root;

    int cx = w / 2;
    int half = w / 2 - 28;  // leave room for the end tick labels
    // The strip has three stacked rows: a top label row (left | readout | right),
    // the axis baseline below it, and the numeric ticks in a reserved gutter under
    // the axis. The end captions live ABOVE the axis so they never collide with
    // the end ticks. base_y pushed down to make room for the top label row.
    int label_row_y = 0;
    int base_y = 18;
    xs.center_x = cx;
    xs.half_px = half;

    // Baseline.
    lv_obj_t *line = lv_obj_create(root);
    lv_obj_set_size(line, half * 2, 2);
    lv_obj_set_pos(line, cx - half, base_y);
    lv_obj_set_style_bg_color(line, lv_color_hex(theme.grid), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    no_chrome(line);

    // Scale ticks + numeric labels at -fs, -fs/2, 0, fs/2, fs. Numeric labels sit
    // in the gutter below the baseline; the top row above carries the end captions.
    const float fr[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        int tx = cx + (int)(fr[i] * half);
        bool mid = (i == 2);
        lv_obj_t *t = lv_obj_create(root);
        lv_obj_set_size(t, 2, mid ? 14 : 8);
        lv_obj_set_pos(t, tx - 1, base_y - (mid ? 10 : 6));
        lv_obj_set_style_bg_color(t, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        no_chrome(t);
        char tbuf[12];
        if (mid)
            snprintf(tbuf, sizeof(tbuf), "0");
        else
            snprintf(tbuf, sizeof(tbuf), "%.*f", tick_decimals, fr[i] * full_scale);
        lv_obj_t *nl = lv_label_create(root);
        lv_label_set_text(nl, tbuf);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_pos(nl, tx - (int)strlen(tbuf) * 4, base_y + 5);
    }

    // End captions in the top label row, ABOVE the axis -- clear of the end ticks
    // (which now live in the gutter below the baseline).
    lv_obj_t *port = lv_label_create(root);
    lv_label_set_text(port, left_label);
    lv_obj_set_style_text_font(port, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(port, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(port, 0, label_row_y);
    lv_obj_t *stbd = lv_label_create(root);
    lv_label_set_text(stbd, right_label);
    lv_obj_set_style_text_font(stbd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(stbd, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(stbd, LV_ALIGN_TOP_RIGHT, 0, label_row_y);

    // Numeric readout centered in the top label row between the end captions, so
    // the operator can read the magnitude, not just which side the needle is on.
    // Caller fills it via set_text_if_changed in the refresh path.
    lv_obj_t *val = lv_label_create(root);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(theme.fg), 0);
    lv_obj_align(val, LV_ALIGN_TOP_MID, 0, label_row_y);
    xs.value = val;

    // Deviation needle, centered (zero) at build time.
    lv_obj_t *needle = lv_obj_create(root);
    lv_obj_set_size(needle, 3, h - 6);
    lv_obj_set_pos(needle, cx - 1, 3);
    lv_obj_set_style_bg_color(needle, lv_color_hex(needle_color), 0);
    lv_obj_set_style_bg_opa(needle, LV_OPA_COVER, 0);
    no_chrome(needle);
    lv_obj_set_style_radius(needle, 1, 0);
    xs.needle = needle;

    return xs;
}

XteStrip build_xte_strip(lv_obj_t *parent, int x, int y, int w, int h) {
    // Cross-track-error: PORT..STBD, ±1.0 nm full-scale (one decimal), red needle.
    return build_centerzero_strip(parent, x, y, w, h, "PORT", "STBD", 1.0, 1, theme.alarm);
}

}  // namespace ui
