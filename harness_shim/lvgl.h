#pragma once
//
// Minimal <lvgl.h> shim for the HEADLESS protocol harness build only
// (YEYBOATS_HARNESS). It is on the include path solely for that env via
// `-I harness_shim`. The harness links NO LVGL: every LVGL-using .cpp TU
// (src/main.cpp, src/ui/*, layout_renderer, manager_screens, ...) is excluded
// from the harness build_src_filter. But several infra TUs that the harness DOES
// link (app_events.cpp, manager.cpp, knob_remote.cpp, proto_target.cpp,
// widget_registry.cpp) transitively #include ui_screens.h / ui_theme.h /
// widget_registry.h, which pull in <lvgl.h> for type names in declarations.
//
// This shim provides just enough opaque types and trivial inline helpers for
// those headers to PARSE. No LVGL symbol is ever referenced at link time by the
// harness (the only inline helpers that touch these - ui::c(), ui_theme styling
// - live in headers that are only instantiated by the excluded UI TUs).
//
// Do NOT add this dir to any display/knob/native env include path.

#include <stdint.h>

// Opaque object handle. LVGL's real lv_obj_t is an incomplete type used only
// via pointer in declarations the harness sees.
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_font_t lv_font_t;

// Color value. The real lv_color_t is a small struct; a single-field struct is
// layout-irrelevant here since nothing is rendered.
typedef struct {
    uint16_t full;
} lv_color_t;

static inline lv_color_t lv_color_hex(uint32_t hex) {
    (void)hex;
    lv_color_t c;
    c.full = 0;
    return c;
}

// Declarations referenced by name in linked infra headers. Never called by the
// harness (the call sites are in excluded UI TUs); declaring them is enough for
// the headers to compile.
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_layer_top(void);
void lv_screen_load(lv_obj_t *scr);
lv_obj_t *lv_bar_create(lv_obj_t *parent);
