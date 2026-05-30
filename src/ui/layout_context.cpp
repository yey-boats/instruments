#include "board.h"

namespace ui {

LayoutContext layout_context() {
    board::Geometry g = board::geometry();
    LayoutContext c{};
    c.w = g.width_px;
    c.h = g.height_px;
    c.short_side = g.width_px < g.height_px ? g.width_px : g.height_px;
    c.long_side = g.width_px > g.height_px ? g.width_px : g.height_px;
    c.square = g.square;
    c.landscape = !g.square && g.width_px > g.height_px;
    c.wide = g.diagonal_tenths_in >= 70 || g.width_px >= 800;
    c.margin = c.short_side >= 800 ? 16 : 8;
    c.gap = c.short_side >= 800 ? 8 : 4;
    // Touch target: 44 px on 480-class panels; 56 px on >= 800-class.
    c.touch_min = c.short_side >= 800 ? 56 : 44;
    return c;
}

}  // namespace ui
