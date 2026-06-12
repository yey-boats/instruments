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
};

// Glass-cockpit chrome metrics (consolidated; no inline magic numbers in
// painters). Single source for radius/border/padding so device and editor
// stay in lockstep.
namespace chrome {
constexpr int panel_radius = 10;
constexpr int panel_border = 1;
constexpr int panel_pad = 10;
constexpr int badge_radius = 6;
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
