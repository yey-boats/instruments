#pragma once

// Dirty-value cache helpers for screen refresh paths
// (docs/specs/09 section "Dirty-Value Caches").
//
// Each screen module declares per-label/per-widget static caches; the
// refresh function reads them through these inlines, which skip the
// underlying LVGL setter when the displayed value would not change.
// LVGL's partial render mode then has nothing to invalidate, and the
// SW renderer/flush callback stays idle.
//
// Sentinels:
//   text cache  : first byte 0xFF means "unset"; first refresh always
//                 writes regardless of `value` content.
//   rot cache   : INT16_MIN means "unset".
//   hidden cache: -1 = unset, 0 = shown, 1 = hidden.

#include <lvgl.h>
#include <stdint.h>
#include <string.h>

namespace ui {

inline void set_text_if_changed(lv_obj_t *obj, char *cache, size_t cap,
                                const char *value) {
    if (!obj || !cache || cap == 0 || !value) return;
    if (strncmp(cache, value, cap) != 0) {
        strncpy(cache, value, cap - 1);
        cache[cap - 1] = 0;
        lv_label_set_text(obj, value);
    }
}

inline void set_rot_if_changed(lv_obj_t *obj, int16_t *cache, int16_t value) {
    if (!obj || !cache) return;
    if (*cache != value) {
        *cache = value;
        lv_obj_set_style_transform_rotation(obj, value, 0);
    }
}

inline void set_hidden_if_changed(lv_obj_t *obj, int8_t *cache, bool hidden) {
    if (!obj || !cache) return;
    int8_t want = hidden ? 1 : 0;
    if (*cache != want) {
        *cache = want;
        if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// Convenience: color/opacity changes invalidate large regions, so
// gate them too. These are slightly more expensive to track because
// colors are 32-bit-ish, but the gain is large for screens that
// repaint a button background or label color on data changes.
inline void set_bg_color_if_changed(lv_obj_t *obj, uint32_t *cache,
                                    uint32_t color) {
    if (!obj || !cache) return;
    if (*cache != color) {
        *cache = color;
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    }
}

inline void set_text_color_if_changed(lv_obj_t *obj, uint32_t *cache,
                                      uint32_t color) {
    if (!obj || !cache) return;
    if (*cache != color) {
        *cache = color;
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    }
}

}  // namespace ui
