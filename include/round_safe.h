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

struct TilePlan {
    bool hidden;  // cell (almost) entirely under the bezel: panel only
    IRect cap;    // caption / content rect (safe rect), screen coordinates
    IRect body;   // painter rect: == cap, or the fitted dial square
};

// Shrink a fractional rect to the integer pixel rect inside it.
IRect inner_pixels(const Rect &r);

// Plan one tile's content on a round panel with visible disc `d`. `dial`
// selects the fitted circle (compass / windrose); `caption_len` is the
// caption's character count (0 = no caption, nothing to avoid).
TilePlan plan_tile(const IRect &cell, const Disc &d, bool dial, size_t caption_len);

}  // namespace round_safe
}  // namespace ui
