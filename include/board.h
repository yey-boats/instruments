#pragma once

// Board abstraction per docs/specs/13-board-display-portability.md.
//
// First-phase scope: identity, geometry, capabilities and backlight.
// Display/touch driver migration comes in follow-ups; for now main.cpp
// keeps owning Arduino_GFX + GT911 init while board.h gives a stable
// API for everything else (settings, ui_layouts, future portability).
//
// Selection at compile time via build_flags:
//   -D BOARD_ID_SUNTON_4848S040    (default if no flag)
//   -D BOARD_ID_WAVESHARE_TOUCH_LCD_4
//
// Common headers / code pull in this file. Board impls live under
// src/boards/board_<id>.cpp.

#include <stdint.h>

#include "board_pins.h"  // legacy LCD_W/LCD_H + pin macros (compat)

namespace board {

enum class DisplayBus : uint8_t { RgbParallel, Spi, Qspi, DsiBridge };
enum class TouchKind : uint8_t { None, GT911, FT5x06, CST816, XPT2046, BoardSpecific };
enum class BacklightKind : uint8_t { None, LedcPwm, IoExpanderPwm, PanelCommand };
enum class DisplayShape : uint8_t { Rectangle, Square, Round };
enum class DensityClass : uint8_t { Mdpi, Hdpi };
enum class LayoutClass : uint8_t {
    SquareCompact,
    LandscapeCompact,
    LandscapeWide,
    PortraitCompact,
    PortraitTall,
};

struct Geometry {
    uint16_t width_px;
    uint16_t height_px;
    uint16_t diagonal_tenths_in;
    uint8_t rotation;
    bool square;
    DisplayShape shape;
    LayoutClass layout_class;
    DensityClass density_class;
    uint16_t usable_x;
    uint16_t usable_y;
    uint16_t usable_width;
    uint16_t usable_height;
};

struct Capabilities {
    bool psram_required;
    BacklightKind backlight;
    TouchKind touch;
    bool touch_calibration;
    bool beeper;
    bool nmea2000_can;
    bool sd_card;
    DisplayBus display_bus;
    bool touch_interrupt;
};

const char *id();            // stable id string, e.g. "sunton_4848s040"
const char *display_name();  // human readable

Geometry geometry();
Capabilities capabilities();

// Backlight control. value is 0..255 normalized (board impl maps to
// whatever PWM/expander/command the panel uses).
bool set_backlight(uint8_t value_0_255);
uint8_t backlight();

// Optional power gate (for boards that route panel power through a GPIO
// or expander). No-op on boards without it.
bool set_power(bool on);

const char *shape_name(DisplayShape shape);
const char *density_class_name(DensityClass density);
const char *layout_class_name(LayoutClass layout);
const char *touch_kind_name(TouchKind touch);
const char *display_bus_name(DisplayBus bus);

// Console handler - "board" or "board bright <0-255>".
bool handleSerialCommand(const class String &line);

// Read the SoC's internal junction-temperature sensor (degrees Celsius).
// Returns NaN if the board build doesn't support it. The sensor has a
// ~10C absolute accuracy but is the right tool for diagnosing thermal
// runaway (hot WiFi/radio die under heavy load) - the relative trend
// is what matters, not the absolute value.
float chipTempC();

}  // namespace board

namespace ui {

// Runtime layout context derived from board::geometry(). Screens use
// this instead of LCD_W/LCD_H so the same screen code works across
// 480x480, 800x480 and portrait boards.
struct LayoutContext {
    uint16_t w;
    uint16_t h;
    uint16_t short_side;
    uint16_t long_side;
    bool square;
    bool landscape;
    bool wide;
    uint16_t margin;
    uint16_t gap;
    uint16_t touch_min;
};

LayoutContext layout_context();

}  // namespace ui
