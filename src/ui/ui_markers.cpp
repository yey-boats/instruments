#include "ui_markers.h"

#include <esp_heap_caps.h>
#include <math.h>

#include "ui_dirty.h"

namespace ui {

static constexpr int GBOX = 28;    // glyph canvas side
static constexpr int MARGIN = 18;  // holder extends this far past the rim
static constexpr double kSemicircleHalfWindowDeg =
    96.0;  // matches the |rel|>96 degree-label hide in ui_compass.cpp / marker_math.h

// One marker holder: a transparent square concentric with the dial, pivoting at
// its center, carrying a glyph canvas at its top edge (pointing inward).
static lv_obj_t *make_holder(lv_obj_t *parent, int cx, int cy, int r) {
    int side = 2 * r + 2 * MARGIN;
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_set_size(h, side, side);
    lv_obj_set_pos(h, cx - side / 2, cy - side / 2);
    lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(h, side / 2, 0);
    lv_obj_set_style_transform_pivot_y(h, side / 2, 0);
    return h;
}

// Free the PSRAM canvas buffer when its canvas object is deleted. LVGL does
// NOT own a user-supplied canvas buffer, and MIDL screens are now rebuilt live
// (theme switch / config push -> apply_all -> reset_screens -> lv_obj_delete),
// so without this every rebuild leaked one buffer per glyph. LV_EVENT_DELETE
// fires on every object in a deleted subtree, covering both whole-screen
// teardown and any future targeted canvas delete. Classic built-once screens
// simply never fire it — behavior there is unchanged.
static void free_canvas_buf_cb(lv_event_t *e) {
    void *buf = lv_event_get_user_data(e);
    if (buf) heap_caps_free(buf);
}

lv_obj_t *draw_glyph(lv_obj_t *parent, Glyph g, bool filled, uint32_t color) {
    uint32_t *buf = (uint32_t *)heap_caps_malloc(GBOX * GBOX * 4, MALLOC_CAP_SPIRAM);
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_obj_set_size(cv, GBOX, GBOX);
    if (!buf) return cv;  // PSRAM exhausted: empty canvas, no crash
    // Tie the buffer's lifetime to the canvas object (freed on delete).
    lv_obj_add_event_cb(cv, free_canvas_buf_cb, LV_EVENT_DELETE, buf);
    lv_canvas_set_buffer(cv, buf, GBOX, GBOX, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_color_t col = lv_color_hex(color);

    auto line = [&](int x0, int y0, int x1, int y1, int w = 3) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = col;
        d.width = w;
        d.round_start = d.round_end = 1;
        d.p1.x = x0;
        d.p1.y = y0;
        d.p2.x = x1;
        d.p2.y = y1;
        lv_draw_line(&layer, &d);
    };
    auto tri = [&](lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c) {
        if (filled) {
            lv_draw_triangle_dsc_t d;
            lv_draw_triangle_dsc_init(&d);
            d.color = col;
            d.opa = LV_OPA_COVER;
            d.p[0] = a;
            d.p[1] = b;
            d.p[2] = c;
            lv_draw_triangle(&layer, &d);
        } else {
            line(a.x, a.y, b.x, b.y);
            line(b.x, b.y, c.x, c.y);
            line(c.x, c.y, a.x, a.y);
        }
    };

    switch (g) {
    case Glyph::Triangle:
        tri({14, 4}, {4, 24}, {24, 24});
        break;
    case Glyph::Diamond:
        if (filled) {
            tri({14, 4}, {24, 14}, {14, 24});
            tri({14, 4}, {4, 14}, {14, 24});
        } else {
            line(14, 4, 24, 14);
            line(24, 14, 14, 24);
            line(14, 24, 4, 14);
            line(4, 14, 14, 4);
        }
        break;
    case Glyph::Circle: {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = col;
        d.center.x = 14;
        d.center.y = 14;
        d.radius = 10;
        d.start_angle = 0;
        d.end_angle = 360;
        d.width = filled ? 10 : 3;
        lv_draw_arc(&layer, &d);
        break;
    }
    case Glyph::Bar: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = col;
        d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
        d.border_color = col;
        d.border_width = filled ? 0 : 2;
        lv_area_t a = {12, 4, 16, 24};
        lv_draw_rect(&layer, &d, &a);
        break;
    }
    case Glyph::Cross: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = col;
        d.bg_opa = filled ? LV_OPA_COVER : LV_OPA_TRANSP;
        d.border_color = col;
        d.border_width = filled ? 0 : 2;
        lv_area_t v = {12, 4, 16, 24};
        lv_area_t hh = {4, 12, 24, 16};
        lv_draw_rect(&layer, &d, &v);
        lv_draw_rect(&layer, &d, &hh);
        break;
    }
    case Glyph::ChevronIn: {
        int lw = filled ? 5 : 3;
        line(4, 4, 14, 16, lw);
        line(14, 16, 24, 4, lw);
        break;
    }
    case Glyph::ChevronOut: {
        int lw = filled ? 5 : 3;
        line(4, 24, 14, 12, lw);
        line(14, 12, 24, 24, lw);
        break;
    }
    case Glyph::ChevronLeft: {
        int lw = filled ? 5 : 3;
        line(18, 4, 8, 14, lw);
        line(8, 14, 18, 24, lw);
        break;
    }
    case Glyph::ChevronRight: {
        int lw = filled ? 5 : 3;
        line(10, 4, 20, 14, lw);
        line(20, 14, 10, 24, lw);
        break;
    }
    case Glyph::ChevronDouble: {
        int lw = filled ? 5 : 3;
        line(13, 4, 3, 14, lw);
        line(3, 14, 13, 24, lw);
        line(25, 4, 15, 14, lw);
        line(15, 14, 25, 24, lw);
        break;
    }
    default:
        break;
    }

    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

MarkerRing build_marker_ring_radii(lv_obj_t *parent, int cx, int cy, const int *radii,
                                   const MarkerSpec *specs, uint8_t count, bool occlude_lower) {
    MarkerRing ring = {};
    ring.occlude_lower = occlude_lower;
    if (count > kMaxMarkersPerDial) count = kMaxMarkersPerDial;
    ring.count = count;
    for (uint8_t i = 0; i < count; ++i) {
        int r = radii[i];
        if (r > ring.r) ring.r = r;
        lv_obj_t *h = make_holder(parent, cx, cy, r);
        lv_obj_t *cv = draw_glyph(h, specs[i].glyph, specs[i].filled, specs[i].color);
        lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, MARGIN - GBOX - 2);
        lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);  // shown on first update
        ring.holder[i] = h;
        ring.last_rot[i] = INT16_MIN;
        ring.last_hidden[i] = -1;
    }
    return ring;
}

MarkerRing build_marker_ring(lv_obj_t *parent, int cx, int cy, int r, const MarkerSpec *specs,
                             uint8_t count, bool occlude_lower) {
    int radii[kMaxMarkersPerDial];
    if (count > kMaxMarkersPerDial) count = kMaxMarkersPerDial;
    for (uint8_t i = 0; i < count; ++i)
        radii[i] = r;
    MarkerRing ring = build_marker_ring_radii(parent, cx, cy, radii, specs, count, occlude_lower);
    ring.r = r;
    return ring;
}

void marker_ring_update(MarkerRing &ring, const MarkerSpec *specs, uint8_t count,
                        double reference_deg) {
    if (count > ring.count) count = ring.count;
    double half = kSemicircleHalfWindowDeg;
    for (uint8_t i = 0; i < count; ++i) {
        double sa = marker_screen_angle(specs[i].value_deg, reference_deg);
        bool hide = isnan(sa) || (ring.occlude_lower && marker_occluded(sa, half));
        set_hidden_if_changed(ring.holder[i], &ring.last_hidden[i], hide);
        if (hide) continue;
        int16_t rot = (int16_t)(lround(sa) * 10 % 3600);
        set_rot_if_changed(ring.holder[i], &ring.last_rot[i], rot);
    }
}

}  // namespace ui
