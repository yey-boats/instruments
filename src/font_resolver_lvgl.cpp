// Arduino-only TU: maps a resolved font size (uint16_t) to an
// LVGL font pointer. The pure resolver lives in font_resolver.cpp;
// this file just owns the size -> lv_font_t binding so the native
// test env doesn't need LVGL at link time.

#include "font_resolver.h"

#include <lvgl.h>

namespace font_resolver {

const lv_font_t *font_for_size(uint16_t size) {
    // Walk through the compiled-in UI fonts (B612, font_b612_*) in
    // increasing size. Update this switch whenever tools/fonts/gen_fonts.sh
    // changes the generated sizes. The host-side DEFAULT_SIZES table must stay
    // in sync.
    switch (size) {
    case 14:
        return &font_b612_14;
    case 20:
        return &font_b612_20;
    case 28:
        return &font_b612_28;
    case 48:
        return &font_b612_48;
    default:
        return &font_b612_14;
    }
}

}  // namespace font_resolver
