#pragma once

// clang-format off
//
// Host (native) LVGL config for the resolution test harness. Mirrors the
// device include/lv_conf.h (same widgets/fonts so layouts render identically)
// but uses the C-library allocator instead of the PSRAM heap_caps allocator,
// and does NOT enable SDL: the harness renders headlessly to a memory
// framebuffer and reads it back with lv_snapshot_take(), so it runs in CI
// without a display server.

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// Host allocator: plain malloc/free via LVGL's clib backend (no heap_caps).
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_MEM_SIZE (16U * 1024U * 1024U)

#define LV_DEF_REFR_PERIOD  16
#define LV_DPI_DEF 130

// Provide ticks from the harness via lv_tick_inc() (LV_TICK_CUSTOM off).
#define LV_TICK_CUSTOM 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_LOG          0

// Widgets (match device)
#define LV_USE_LABEL    1
#define LV_USE_BUTTON   1
#define LV_USE_OBJ      1
#define LV_USE_LINE     1
#define LV_USE_ARC      1
#define LV_USE_BAR      1
#define LV_USE_SCALE    1
#define LV_USE_METER    0
#define LV_USE_CHART    1
#define LV_USE_SLIDER   1
#define LV_USE_SWITCH   1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE    0
#define LV_USE_IMAGE    1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_SPINBOX     0
#define LV_USE_KEYBOARD    1
#define LV_USE_CANVAS      1
#define LV_USE_QRCODE      1

// Themes
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_THEME_SIMPLE 1

// Layouts
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// Fonts (match device)
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_48 0
// The UI uses B612 (src/fonts/font_b612_*.c, tools/fonts/gen_fonts.sh) in
// place of the built-in Montserrat, disabled above. Declared here so every
// file that includes lvgl.h sees them.
#define LV_FONT_CUSTOM_DECLARE                                                                     \
    LV_FONT_DECLARE(font_b612_14) LV_FONT_DECLARE(font_b612_20) LV_FONT_DECLARE(font_b612_28)       \
        LV_FONT_DECLARE(font_b612_38) LV_FONT_DECLARE(font_b612_48)
#define LV_FONT_DEFAULT &font_b612_14

// Misc
#define LV_USE_ANIMATION 1
#define LV_USE_SHADOW    1
#define LV_USE_OUTLINE   1
#define LV_USE_BLEND_MODES 1

// Snapshot: the harness renders screens to a buffer for PNG/BMP export.
#define LV_USE_SNAPSHOT 1
