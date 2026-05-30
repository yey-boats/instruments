// Arduino-only TU: maps a resolved font size (uint16_t) to an
// LVGL font pointer. The pure resolver lives in font_resolver.cpp;
// this file just owns the size -> lv_font_t binding so the native
// test env doesn't need LVGL at link time.

#include "font_resolver.h"

#include <lvgl.h>

namespace font_resolver {

const lv_font_t *font_for_size(uint16_t size) {
    // Walk through compiled-in Montserrat fonts in increasing size.
    // Update this switch whenever lv_conf.h LV_FONT_MONTSERRAT_*
    // enables change. The host-side DEFAULT_SIZES table must stay
    // in sync.
    switch (size) {
    case 14:
        return &lv_font_montserrat_14;
    case 20:
        return &lv_font_montserrat_20;
    case 28:
        return &lv_font_montserrat_28;
    case 48:
        return &lv_font_montserrat_48;
    default:
        return &lv_font_montserrat_14;
    }
}

}  // namespace font_resolver
