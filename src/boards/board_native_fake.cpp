// Native-test fake board impl. Spec 13 step 9.
//
// Compiled only when BOARD_ID_NATIVE_FAKE is set (the `native` test
// env). Returns deterministic identity / geometry / capabilities so
// host tests for board::* and ui::LayoutContext can run without any
// Arduino, LEDC, or GPIO dependencies.
//
// The geometry can be overridden at compile time via build flags so a
// single test source can exercise multiple board shapes:
//   -DFAKE_BOARD_WIDTH=800 -DFAKE_BOARD_HEIGHT=480
// The defaults are 480x480 to match the production board.

#if defined(BOARD_ID_NATIVE_FAKE)

#include "board.h"
#include "boards/board_native_fake.h"

#ifndef FAKE_BOARD_WIDTH
#define FAKE_BOARD_WIDTH 480
#endif
#ifndef FAKE_BOARD_HEIGHT
#define FAKE_BOARD_HEIGHT 480
#endif
#ifndef FAKE_BOARD_DIAG_TENTHS
#define FAKE_BOARD_DIAG_TENTHS 40
#endif

namespace board {

namespace {

uint8_t s_backlight_value = 255;
bool s_power = true;
uint16_t s_width = FAKE_BOARD_WIDTH;
uint16_t s_height = FAKE_BOARD_HEIGHT;
uint16_t s_diag = FAKE_BOARD_DIAG_TENTHS;
DisplayShape s_shape =
    FAKE_BOARD_WIDTH == FAKE_BOARD_HEIGHT ? DisplayShape::Square : DisplayShape::Rectangle;

LayoutClass classify(uint16_t w, uint16_t h) {
    if (w == h) return LayoutClass::SquareCompact;
    if (w > h) {
        return (w >= 1024 || h >= 600) ? LayoutClass::LandscapeWide : LayoutClass::LandscapeCompact;
    }
    return (uint32_t)h * 10 / w >= 15 ? LayoutClass::PortraitTall : LayoutClass::PortraitCompact;
}

void fill_usable_area(Geometry &g) {
    if (g.shape != DisplayShape::Round) {
        g.usable_x = 0;
        g.usable_y = 0;
        g.usable_width = g.width_px;
        g.usable_height = g.height_px;
        return;
    }
    const uint16_t inset = g.width_px >= 480 ? 48 : 42;
    g.usable_x = inset;
    g.usable_y = inset;
    g.usable_width = g.width_px > inset * 2 ? g.width_px - inset * 2 : g.width_px;
    g.usable_height = g.height_px > inset * 2 ? g.height_px - inset * 2 : g.height_px;
}

}  // namespace

const char *id() {
    return "native_fake";
}
const char *display_name() {
    return "Native test fake board";
}

Geometry geometry() {
    Geometry g{};
    g.width_px = s_width;
    g.height_px = s_height;
    g.diagonal_tenths_in = s_diag;
    g.rotation = 0;
    g.square = (g.width_px == g.height_px);
    g.shape = s_shape;
    g.layout_class = classify(g.width_px, g.height_px);
    g.density_class =
        (g.width_px >= 1024 || g.height_px >= 600) ? DensityClass::Hdpi : DensityClass::Mdpi;
    fill_usable_area(g);
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = false;
    c.backlight = BacklightKind::None;
    c.touch = TouchKind::None;
    c.touch_calibration = false;
    c.beeper = false;
    c.nmea2000_can = false;
    c.sd_card = false;
    c.display_bus = DisplayBus::RgbParallel;
    c.touch_interrupt = false;
    return c;
}

bool set_backlight(uint8_t value_0_255) {
    s_backlight_value = value_0_255;
    return true;
}

uint8_t backlight() {
    return s_backlight_value;
}

bool set_power(bool on) {
    s_power = on;
    return true;
}

namespace native_fake {

void set_geometry(uint16_t width_px, uint16_t height_px, uint16_t diagonal_tenths_in) {
    const uint16_t next_w = width_px ? width_px : s_width;
    const uint16_t next_h = height_px ? height_px : s_height;
    set_geometry(width_px, height_px, diagonal_tenths_in,
                 next_w == next_h ? DisplayShape::Square : DisplayShape::Rectangle);
}

void set_geometry(uint16_t width_px, uint16_t height_px, uint16_t diagonal_tenths_in,
                  DisplayShape shape) {
    if (width_px) s_width = width_px;
    if (height_px) s_height = height_px;
    if (diagonal_tenths_in) s_diag = diagonal_tenths_in;
    s_shape = shape;
}

void reset_geometry() {
    s_width = FAKE_BOARD_WIDTH;
    s_height = FAKE_BOARD_HEIGHT;
    s_diag = FAKE_BOARD_DIAG_TENTHS;
    s_shape =
        FAKE_BOARD_WIDTH == FAKE_BOARD_HEIGHT ? DisplayShape::Square : DisplayShape::Rectangle;
}

}  // namespace native_fake

}  // namespace board

#endif  // BOARD_ID_NATIVE_FAKE
