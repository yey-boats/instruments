#pragma once

#include <lvgl.h>
#include <stdint.h>

// Palette indirection so day/night themes can flip a single struct without
// every widget knowing how to re-style itself.

namespace ui {

struct Palette {
    uint32_t bg;          // screen background
    uint32_t panel;       // card / quadrant background
    uint32_t panel_edge;  // panel border
    uint32_t fg;          // primary text
    uint32_t fg_dim;      // secondary text / units
    uint32_t accent;      // hero metric color
    uint32_t warn;        // amber-ish accent
    uint32_t alarm;       // red / urgent
    uint32_t good;        // green / ok
    uint32_t port;        // red side (port wind)
    uint32_t starboard;   // green side (starboard wind)
    uint32_t grid;        // axis / compass tick color
};

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
