#pragma once

// Round-safe tile placement (instruments#50, port of midl web/src/svg/round-safe.ts,
// midl#36 / midl PR #44).
//
// On a round panel the MIDL solver still lays tiles out in the full square, so
// corner tiles reach under the bezel. The panel background keeps the whole
// cell; only the tile CONTENT moves into the part of the cell that lies inside
// the visible circle:
//   - safe_rect:  the largest axis-aligned rect inside cell ∩ disc (caption,
//                 values, bars, pills);
//   - fit_circle: the largest circle inside cell ∩ disc that avoids a box
//                 (round dials, which need a circle, not a rect).
// Pure geometry (no LVGL / Arduino), host-tested in test/test_round_safe.
// Keep the numeric behaviour in step with midl's round-safe.ts.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>

namespace ui {
namespace round_safe {

struct Rect {
    double x, y, w, h;
};

struct Disc {
    double cx, cy, r;
};

// Visible disc of a round viewport, inset by `margin` px from the bezel.
Disc viewport_disc(const Rect &vp, double margin = 4);

// True when every corner of `t` lies inside the disc.
bool rect_in_disc(const Rect &t, const Disc &d);

// Largest-area axis-aligned rect inside `t` ∩ disc; `t` unchanged when it
// already fits. A cell entirely outside the disc yields a zero-size rect at
// the cell centre.
Rect safe_rect(const Rect &t, const Disc &d);

// Largest circle inside `t` ∩ disc that does not overlap `avoid` (may be
// null), returned as the square circumscribing it (dials size themselves from
// min(w, h)). Never negative.
Rect fit_circle(const Rect &t, const Disc &d, const Rect *avoid = nullptr);

// ---- firmware tile plan --------------------------------------------------
// How build_tile (ui_layouts.cpp) places one tile's content. Mirrors midl's
// paintScreenSvg round branch, with the firmware chrome metrics: the caption
// is montserrat_20 at content (10, 2) inside a style_panel inset of 11 px.

// Smallest round-safe content width worth drawing into (midl MIN_SAFE).
constexpr int kMinSafe = 24;
// Height of the firmware caption row incl. the panel inset (11 + 2 + ~24 px
// montserrat_20 line box + 1); midl's captionBand(180 px cell) is 29 with its
// caption 6 px from the cell top.
constexpr int kCaptionBand = 38;
// Caption box width estimate: inset + 10 px + ~15 px per montserrat_20
// uppercase glyph.
constexpr int kCaptionLeft = 21;
constexpr int kCaptionGlyphW = 15;

struct IRect {
    int x, y, w, h;
};

// Ring-shaped painters (ui_layouts.cpp): the compass and wind-rose tiles and
// the device's arc gauge (an lv_arc ring, unlike the web's flat gauge).
enum class Dial : unsigned char { None, Compass, WindRose, Gauge };

// The ring painters draw dia = min(w, h) - kDialReserve, centred kDialDrop px
// below their box centre (ui::geom::dial_ring_dia / dial_center).
constexpr int kDialReserve = 56;
constexpr int kDialDrop = 4;

// How far a ring painter draws beyond its ring on a round panel: the orbiting
// marker glyphs (pulled 6 px inward there, so their tips reach ring + 11/12 px)
// and the gauge's zone band (ring + 7 px).
constexpr int dial_overhang(Dial k) {
    return k == Dial::Gauge ? 8 : 12;
}
// Smallest ring each painter lays out cleanly (its own floor size). A fitted
// circle that cannot hold it gets the value-only fallback instead of a ring
// that would spill out of the circle into the neighbouring cells.
constexpr int dial_min_ring(Dial k) {
    return k == Dial::Gauge ? 88 : k == Dial::WindRose ? 80 : k == Dial::Compass ? 72 : 0;
}

struct TilePlan {
    bool hidden;   // cell (almost) entirely under the bezel: panel only
    IRect cap;     // caption / content rect (safe rect), screen coordinates
    IRect body;    // painter box: == cap, or the ring painter's box (see ring_dia)
    IRect circle;  // dials: square circumscribing the fitted circle (else zero)
    // > 0: draw the ring at this diameter, concentric with `circle`; ring +
    //      overhang stays inside the circle. `body` is the painter box that
    //      centres it (it is larger than the circle; only the ring is drawn).
    // 0:   not a dial, or the circle is too small for a ring: value-only
    //      content in `cap` (value_only tells the two apart).
    int ring_dia;
    bool value_only;
};

// Shrink a fractional rect to the integer pixel rect inside it.
IRect inner_pixels(const Rect &r);

// The chrome caption's box for a caption of `caption_len` characters placed at
// the top-left of safe rect `cap` (the box a fitted dial has to stay clear of).
IRect caption_box(const IRect &cap, size_t caption_len);

// Plan one tile's content on a round panel with visible disc `d`. `dial`
// selects the fitted circle; `caption_len` is the caption's character count
// (0 = no caption, nothing to avoid).
TilePlan plan_tile(const IRect &cell, const Disc &d, Dial dial, size_t caption_len);

}  // namespace round_safe
}  // namespace ui
