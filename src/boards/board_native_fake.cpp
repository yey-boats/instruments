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

LayoutClass classify(uint16_t w, uint16_t h) {
    if (w == h) return LayoutClass::SquareCompact;
    if (w > h) {
        // wide ratio: long side / short side >= 1.5 -> Wide
        return (uint32_t)w * 10 / h >= 15 ? LayoutClass::LandscapeWide
                                          : LayoutClass::LandscapeCompact;
    }
    return (uint32_t)h * 10 / w >= 15 ? LayoutClass::PortraitTall
                                      : LayoutClass::PortraitCompact;
}

}  // namespace

const char *id() { return "native_fake"; }
const char *display_name() { return "Native test fake board"; }

Geometry geometry() {
    Geometry g{};
    g.width_px = FAKE_BOARD_WIDTH;
    g.height_px = FAKE_BOARD_HEIGHT;
    g.diagonal_tenths_in = FAKE_BOARD_DIAG_TENTHS;
    g.rotation = 0;
    g.square = (g.width_px == g.height_px);
    g.layout_class = classify(g.width_px, g.height_px);
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
    return c;
}

bool set_backlight(uint8_t value_0_255) {
    s_backlight_value = value_0_255;
    return true;
}

uint8_t backlight() { return s_backlight_value; }

bool set_power(bool on) {
    s_power = on;
    return true;
}

}  // namespace board

#endif  // BOARD_ID_NATIVE_FAKE
