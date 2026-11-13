#pragma once

// Pure (no-LVGL) layout math for screens and tiles: overlap tests, the
// heading-band numeral visibility rule, and the square wind-steer stack.
// Header-only so the LVGL renderers and the host tests (test/test_ui_geom)
// share one implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <math.h>

namespace ui {
namespace geom {

// Half-open axis-aligned box [x0,x1) x [y0,y1).
struct Box {
    int x0, y0, x1, y1;
};

inline bool boxes_overlap(const Box &a, const Box &b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

// True when `b` stays at least `margin` px clear of the disc (cx, cy, r).
inline bool box_clears_disc(const Box &b, int cx, int cy, int r, int margin) {
    // Nearest point of the box to the disc centre.
    long nx = cx < b.x0 ? b.x0 : (cx > b.x1 ? b.x1 : cx);
    long ny = cy < b.y0 ? b.y0 : (cy > b.y1 ? b.y1 : cy);
    long dx = cx - nx, dy = cy - ny;
    long rr = (long)r + margin;
    return dx * dx + dy * dy >= rr * rr;
}

// ---- compass tile legend ------------------------------------------------------
// The round compass tile (ui_layouts.cpp paint_compass_body) can draw a
// three-row HDG/COG/CTS legend in the content box's bottom-left corner. All
// coordinates are content-box coordinates. Constants mirror the renderer.
constexpr int kLegendInset = 4;      // gx: left inset of the swatches
constexpr int kLegendRowH = 16;      // one row: 14 px swatch + montserrat_14 label
constexpr int kLegendW = 14 + 32;    // swatch + "HDG"/"COG"/"CTS" label
constexpr int kLegendTopClear = 24;  // keep below the tile's caption row
constexpr int kMarkerOverhang = 20;  // = ui_markers.cpp GLYPH_OVERHANG (static_assert there)
constexpr int kCtsHalfW = 48;        // half-width of the "CTS 123°" line under the ring
constexpr int kCtsH = 20;            // its height incl. the 2 px gap to the ring

// Round dial geometry shared by the compass and wind-rose tiles: ring diameter
// min(w, h) - 56 (floor `min_dia`), centred in the content box and nudged 4 px
// down (the ring's LV_ALIGN_CENTER, 0, +4). `inset` = border + pad.
inline int dial_ring_dia(int w, int h, int min_dia) {
    int dia = (w < h ? w : h) - 56;
    return dia < min_dia ? min_dia : dia;
}
inline void dial_center(int w, int h, int inset, int &cx, int &cy) {
    cx = (w - 2 * inset) / 2;
    cy = (h - 2 * inset) / 2 + 4;
}

// Top row y of the legend in a content box `content_h` tall.
inline int compass_legend_top(int content_h) {
    return content_h - 3 * kLegendRowH - 2;
}

// Whether the legend fits: inside the content box below the caption, clear of
// the ring plus its orbiting marker glyphs, and clear of the CTS line that
// tracks the ring's bottom. (mcx, mcy) is the ring centre, `dia` its diameter.
inline bool compass_legend_fits(int content_w, int content_h, int mcx, int mcy, int dia) {
    const int gy = compass_legend_top(content_h);
    // Each row is a 14 px swatch (row..row+14) beside a 16 px label.
    const Box legend = {kLegendInset, gy, kLegendInset + kLegendW, gy + 3 * kLegendRowH};
    if (legend.y0 < kLegendTopClear || legend.x1 > content_w) return false;
    if (!box_clears_disc(legend, mcx, mcy, dia / 2, kMarkerOverhang)) return false;
    const int ring_bottom = mcy + dia / 2;
    const Box cts = {mcx - kCtsHalfW, ring_bottom, mcx + kCtsHalfW, ring_bottom + kCtsH};
    return !boxes_overlap(legend, cts);
}

// ---- heading band numerals ------------------------------------------------------
// The rolling heading band (ui_compass.cpp compass_layout_labels) shows a degree
// numeral only where it sits fully on the band (|rel| <= 80 deg) and not under
// the lubber (|rel| >= 12 deg), where the HDG/COG/CTS markers cluster. A numeral
// that is currently hidden must clear each limit by kBandHysteresisDeg before it
// reappears, so a heading held near a limit (e.g. ~012 deg) cannot make it
// flicker on every refresh. A numeral not laid out yet (after a build or theme
// switch) uses the plain limits, so the first frame does not depend on history.
constexpr double kBandEdgeDeg = 80.0;
constexpr double kBandLubberDeg = 12.0;
constexpr double kBandHysteresisDeg = 3.0;

enum class BandLabel { New, Shown, Hidden };

// `rel` = numeral bearing - heading, in [-180, 180]. Returns the new visibility.
inline bool band_numeral_visible(double rel, BandLabel prev) {
    const double a = fabs(rel);
    const double slack = prev == BandLabel::Hidden ? kBandHysteresisDeg : 0.0;
    return a <= kBandEdgeDeg - slack && a >= kBandLubberDeg + slack;
}

// ---- square wind-steer stack ---------------------------------------------------
// screen_wind_steer.cpp on a non-wide panel stacks, top to bottom: the top bar,
// the semicircular compass, the XTE strip, a row of four wind tiles and the
// course-adjust row. The compass is as wide as the panel allows; on short
// panels it shrinks until the tiles keep kSteerMinTileH, and when even the
// smallest compass leaves no room the tiles are dropped (and the compass sized
// so the XTE strip still clears the course row). Note: 240x240 lands on that
// last path with a compass too small for the hero inside its band (the screen
// stays free of overlapping rows and negative sizes, but a dedicated small-panel
// layout is still needed there).
constexpr int kSteerTopBarH = 56;
constexpr int kSteerXteGap = 4;  // compass bottom -> XTE strip
constexpr int kSteerXteH = 44;
constexpr int kSteerStripPad = 2;  // XTE strip bottom -> tiles
constexpr int kSteerGap = 8;
constexpr int kSteerCourseH = 36;
constexpr int kSteerCoursePad = 6;
constexpr int kSteerMinTileH = 68;      // 8 px pads + caption + a 28 px value
constexpr int kSteerMinCompassW = 120;  // below this the band is unreadable

struct SteerSquareLayout {
    int compass_w;  // width passed to ui::build_compass
    bool tiles;     // false: no room for the tile row
    int tile_w, tile_h, tile_y;
};

// Height ui::build_compass gives a compass `w` wide (r + 24, r = w/2 - 8);
// keep in lockstep with build_compass in ui_compass.cpp.
inline int steer_compass_h(int w) {
    return (w / 2 - 8) + 24;
}

// Bottom of the XTE strip (incl. its pad) under a compass `w` wide.
inline int steer_strip_bottom(int w) {
    return kSteerTopBarH + steer_compass_h(w) + kSteerXteGap + kSteerXteH + kSteerStripPad;
}

inline SteerSquareLayout wind_steer_square_layout(int lcd_w, int lcd_h) {
    SteerSquareLayout L = {};
    L.tile_w = (lcd_w - kSteerGap * 5) / 4;
    const int tile_bottom = lcd_h - kSteerGap - kSteerCourseH - kSteerCoursePad;
    int cw = lcd_w - 32;
    int room = tile_bottom - steer_strip_bottom(cw);
    if (room < kSteerMinTileH) cw -= 2 * (kSteerMinTileH - room);  // 2 px width per px height
    if (cw >= kSteerMinCompassW) {
        room = tile_bottom - steer_strip_bottom(cw);
        L.tiles = true;
        L.tile_h = room < L.tile_w ? room : L.tile_w;
        L.tile_y = tile_bottom - L.tile_h;
    } else {
        // No tile row: the largest compass whose XTE strip clears the course row.
        const int course_top = lcd_h - kSteerCourseH - kSteerCoursePad;
        cw = lcd_w - 32;
        const int over = steer_strip_bottom(cw) - course_top;
        if (over > 0) cw -= 2 * over;
        L.tiles = false;
    }
    L.compass_w = cw;
    return L;
}

}  // namespace geom
}  // namespace ui
