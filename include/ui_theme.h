#pragma once

#include <lvgl.h>
#include <stdint.h>

// Palette indirection so day/night themes can flip a single struct without
// every widget knowing how to re-style itself.

namespace ui {

struct Palette {
    uint32_t bg;          // screen background
    uint32_t panel;       // card / quadrant background (gradient top)
    uint32_t panel_bot;   // card gradient bottom (glass-cockpit depth)
    uint32_t panel_edge;  // panel border
    uint32_t badge;       // unit-badge background
    uint32_t fg;          // primary text
    uint32_t fg_dim;      // secondary text / units / labels
    uint32_t accent;      // hero metric color
    uint32_t warn;        // amber-ish accent
    uint32_t alarm;       // red / urgent
    uint32_t good;        // green / ok
    uint32_t port;        // red side (port wind)
    uint32_t starboard;   // green side (starboard wind)
    uint32_t grid;        // axis / compass tick color
    uint32_t arc_band;    // near-white gauge band (reference compass rail)
};

// Glass-cockpit chrome metrics (consolidated; no inline magic numbers in
// painters). Single source for radius/border/padding so device and editor
// stay in lockstep.
namespace chrome {
constexpr int panel_radius = 10;
constexpr int panel_border = 1;
constexpr int panel_pad = 10;
constexpr int badge_radius = 6;

// Per-controller "controlled" frame overlay (lv_layer_top()). Each active
// session draws one thin nested border, inset by frame_step * i (outermost =
// most-recent); a small name-pill (top-center) shows the most-recent
// controller. Single source for these so the frame stays in lockstep with the
// rest of the chrome. See src/ui/control_frame.cpp.
namespace control_frame {
constexpr int border_width = 3;     // thickness of each session border ring
constexpr int frame_step = 5;       // inset between stacked rings (px)
constexpr int rect_radius = 12;     // corner radius on rectangular panels
constexpr int pill_pad_x = 10;      // name-pill horizontal padding
constexpr int pill_pad_y = 4;       // name-pill vertical padding
constexpr int pill_radius = 10;     // name-pill corner radius
constexpr int pill_margin_top = 6;  // gap from top edge to the pill
constexpr uint32_t pill_text = 0xffffff;
}  // namespace control_frame
}  // namespace chrome

extern Palette theme;

void use_night();  // dark blue, default
void use_day();    // light / sunlight-readable

inline lv_color_t c(uint32_t hex) {
    return lv_color_hex(hex);
}

void style_screen(lv_obj_t *o);
void style_panel(lv_obj_t *o, uint32_t accent = 0);
void style_caption(lv_obj_t *o);
void style_value(lv_obj_t *o, const lv_font_t *font, uint32_t color);
lv_obj_t *panel_accent(lv_obj_t *parent, uint32_t color);

}  // namespace ui
