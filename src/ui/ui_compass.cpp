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

static lv_obj_t *polar_label(lv_obj_t *group, int gcx, int gcy, const char *txt, int deg,
                             int radius, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(group);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    double a = deg * M_PI / 180.0;
    int x = gcx + (int)(radius * sin(a));
    int y = gcy - (int)(radius * cos(a));
    int hw = (int)strlen(txt) * 6;
    lv_obj_set_pos(l, x - hw, y - 10);
    return l;
}

// ---- compass ----------------------------------------------------------------

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
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, w, r + 18);
    lv_obj_set_pos(root, ox, oy);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    no_chrome(root);
    lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    cp.root = root;

    // Green outer rail, then the white band just inside it.
    band_arc(root, cx, cy, r, 6, theme.good, 180, 360, LV_OPA_COVER);
    band_arc(root, cx, cy, r - 6, 26, theme.arc_band, 180, 360, LV_OPA_COVER);

    // Rotating scale: degree numbers + ticks. Rotated by -heading in refresh.
    lv_obj_t *scale = lv_obj_create(root);
    lv_obj_set_size(scale, r * 2, r * 2);
    lv_obj_set_pos(scale, cx - r, cy - r);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    no_chrome(scale);
    lv_obj_set_style_transform_pivot_x(scale, r, 0);
    lv_obj_set_style_transform_pivot_y(scale, r, 0);
    cp.scale = scale;

    int rim = r - 4;
    for (int deg = 0; deg < 360; deg += 10) {
        bool major = (deg % 30) == 0;
        radial_tick(scale, r, r, deg, major ? 12 : 6, major ? 3 : 2, rim,
                    major ? theme.fg : theme.fg_dim);
    }
    char buf[8];
    for (int deg = 0; deg < 360; deg += 30) {
        const char *txt;
        uint32_t color = theme.fg;
        if (deg == 0) {
            txt = "N";
            color = theme.alarm;
        } else if (deg == 90) {
            txt = "E";
            color = theme.alarm;
        } else if (deg == 180) {
            txt = "S";
            color = theme.alarm;
        } else if (deg == 270) {
            txt = "W";
            color = theme.alarm;
        } else {
            snprintf(buf, sizeof(buf), "%d", deg);
            txt = buf;
        }
        polar_label(scale, r, r, txt, deg, r - 30, &lv_font_montserrat_20, color);
    }

    // Amber target bug riding on the rail; rotated by (target - heading).
    lv_obj_t *bug = lv_obj_create(root);
    lv_obj_set_size(bug, 22, r);
    lv_obj_set_pos(bug, cx - 11, cy - r);
    lv_obj_set_style_bg_opa(bug, LV_OPA_TRANSP, 0);
    no_chrome(bug);
    lv_obj_set_style_transform_pivot_x(bug, 11, 0);
    lv_obj_set_style_transform_pivot_y(bug, r, 0);
    lv_obj_t *bug_tri = lv_label_create(bug);
    lv_label_set_text(bug_tri, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(bug_tri, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bug_tri, lv_color_hex(theme.warn), 0);
    lv_obj_align(bug_tri, LV_ALIGN_TOP_MID, 0, -4);
    cp.bug = bug;

    // Fixed red lubber at the very top: the boat's heading reference.
    lv_obj_t *lub = lv_label_create(root);
    lv_label_set_text(lub, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(lub, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lub, lv_color_hex(theme.alarm), 0);
    lv_obj_align(lub, LV_ALIGN_TOP_MID, 0, 0);

    return cp;
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

XteStrip build_xte_strip(lv_obj_t *parent, int x, int y, int w, int h) {
    XteStrip xs = {};
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, w, h);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    no_chrome(root);
    xs.root = root;

    int cx = w / 2;
    int half = w / 2 - 28;  // leave room for PORT / STBD text
    int base_y = h / 2 + 4;
    xs.center_x = cx;
    xs.half_px = half;

    // Baseline.
    lv_obj_t *line = lv_obj_create(root);
    lv_obj_set_size(line, half * 2, 2);
    lv_obj_set_pos(line, cx - half, base_y);
    lv_obj_set_style_bg_color(line, lv_color_hex(theme.grid), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    no_chrome(line);

    // Scale ticks + numeric labels at -1, -0.5, 0, 0.5, 1.
    static const float fr[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    static const char *lbl[] = {"-1.0", "-0.5", "0", "0.5", "1.0"};
    for (int i = 0; i < 5; ++i) {
        int tx = cx + (int)(fr[i] * half);
        bool mid = (i == 2);
        lv_obj_t *t = lv_obj_create(root);
        lv_obj_set_size(t, 2, mid ? 14 : 8);
        lv_obj_set_pos(t, tx - 1, base_y - (mid ? 10 : 6));
        lv_obj_set_style_bg_color(t, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        no_chrome(t);
        lv_obj_t *nl = lv_label_create(root);
        lv_label_set_text(nl, lbl[i]);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_pos(nl, tx - (int)strlen(lbl[i]) * 4, base_y + 6);
    }

    // PORT / STBD captions at the ends.
    lv_obj_t *port = lv_label_create(root);
    lv_label_set_text(port, "PORT");
    lv_obj_set_style_text_font(port, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(port, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(port, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *stbd = lv_label_create(root);
    lv_label_set_text(stbd, "STBD");
    lv_obj_set_style_text_font(stbd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(stbd, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(stbd, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Red deviation needle, centered (zero) at build time.
    lv_obj_t *needle = lv_obj_create(root);
    lv_obj_set_size(needle, 3, h - 6);
    lv_obj_set_pos(needle, cx - 1, 3);
    lv_obj_set_style_bg_color(needle, lv_color_hex(theme.alarm), 0);
    lv_obj_set_style_bg_opa(needle, LV_OPA_COVER, 0);
    no_chrome(needle);
    lv_obj_set_style_radius(needle, 1, 0);
    xs.needle = needle;

    return xs;
}

}  // namespace ui
