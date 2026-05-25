#pragma once

// clang-format off

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (96U * 1024U)

#define LV_DEF_REFR_PERIOD  16
#define LV_DPI_DEF 130

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
#define LV_USE_CANVAS      1  // required by LV_USE_QRCODE
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
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Misc
#define LV_USE_ANIMATION 1
#define LV_USE_SHADOW    1
#define LV_USE_OUTLINE   1
#define LV_USE_BLEND_MODES 1

// Enable lv_snapshot_take so the web UI can return a BMP of the active
// screen (used by /api/screenshot.bmp for the future design iteration loop).
#define LV_USE_SNAPSHOT 1
