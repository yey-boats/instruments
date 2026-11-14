#pragma once

// clang-format off

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// PSRAM-backed allocators. lv_malloc/free/realloc -> heap_caps_*(SPIRAM)
// via src/lvgl_alloc.cpp. Frees the entire 8 MB PSRAM pool for LVGL,
// removing the LV_MEM_SIZE ceiling. lv_qrcode, lv_canvas, lv_snapshot all
// now have room to allocate big pixmaps.
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
// Ignored when STDLIB_MALLOC == CUSTOM, but some LVGL paths still reference
// it for sanity defaults.
#define LV_MEM_SIZE (96U * 1024U)

#define LV_DEF_REFR_PERIOD  16
#define LV_DPI_DEF 130

// Parallel software rendering. The ST7701 panel has no 2D accelerator, so
// every dirty area is rasterized in software; by default that runs single-
// threaded on the lvgl_task core. Enabling the FreeRTOS OS integration lets
// LVGL split large dirty areas across N draw units (worker tasks), using the
// otherwise-idle second core.
//   LV_USE_OS=FREERTOS spawns the draw-unit worker tasks; lv_timer_handler
//   still runs on the single lvgl_task, so the "all lv_* on one task" rule is
//   unchanged (the workers only touch draw buffers, coordinated internally).
// HARDWARE-RISK / MUST MEASURE: each extra draw unit is a FreeRTOS task whose
// stack is INTERNAL SRAM. The idf5+BLE build already sits at ~1 KB internal
// low-water (see project memory) - a 2nd draw thread can tip it into the
// WiFi/BLE-starvation reboot zone. Also competes with the core-0 touch task.
// To revert: delete these two #defines (falls back to OS_NONE / 1 draw unit).
//
// REVERTED 2026-06-24: the 2-draw-unit build hung TWICE under live SignalK load
// on the lab device (silent hang, AP association FAILED, needed a power-cycle) -
// the predicted internal-SRAM starvation. Falling back to single-threaded
// software rendering (OS_NONE / 1 draw unit) for stability. Re-enable only after
// measuring internal-SRAM low-water headroom under load (idf5+BLE+live data).
// #define LV_USE_OS LV_OS_FREERTOS
// #define LV_DRAW_SW_DRAW_UNIT_CNT 2

#define LV_TICK_CUSTOM 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_LOG          0

// Widgets
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

// Widgets we don't use - explicitly disabled (some depend on textarea)
#define LV_USE_SPINBOX     0
#define LV_USE_KEYBOARD    1
#define LV_USE_CALENDAR    0
#define LV_USE_CHECKBOX    0
#define LV_USE_DROPDOWN    0
#define LV_USE_ROLLER      0
#define LV_USE_LIST        0
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      0
#define LV_USE_SPAN        0
#define LV_USE_TABVIEW     0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_IME_PINYIN  0
#define LV_USE_SPINNER     0
#define LV_USE_LED         0
#define LV_USE_ANIMIMG     0
#define LV_USE_CANVAS      1
#define LV_USE_QRCODE      1
#define LV_USE_BARCODE     0

// Themes
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_THEME_SIMPLE 1

// Layouts
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// Fonts
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

// Snapshot enabled (used by /api/screenshot.bmp). The web task still
// can't call it directly until we marshal via a queue; for now it returns
// 503, but enabling here so we don't have to rebuild lvgl to flip it.
#define LV_USE_SNAPSHOT 1
