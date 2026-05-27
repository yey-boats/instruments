#include "board.h"

#include <Arduino.h>

// Sunton/Guition ESP32-4848S040 implementation of the board::* API.
// First-phase impl: identity, geometry, capability flags, and backlight.
// Display/touch init still live in main.cpp / touch driver - this file
// gives the rest of the codebase a stable façade so future board
// support can plug in without scattering #ifdefs through screens.
//
// Backlight on this board is on GPIO 38, driven directly (no PWM
// channel reserved yet). Until we move LEDC ownership here, set_backlight
// just toggles full on/off above/below 32 so the API contract holds.

#if defined(BOARD_ID_SUNTON_4848S040) || !defined(BOARD_ID_DEFINED)

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = 38;
uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);
    s_backlight_inited = true;
}

}  // namespace

const char *id() { return "sunton_4848s040"; }
const char *display_name() { return "Sunton/Guition ESP32-4848S040 4.0\" 480x480"; }

Geometry geometry() {
    Geometry g{};
    g.width_px = 480;
    g.height_px = 480;
    g.diagonal_tenths_in = 40;
    g.rotation = 0;
    g.square = true;
    g.layout_class = LayoutClass::SquareCompact;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;
    c.backlight = BacklightKind::LedcPwm;  // we drive it manually for now
    c.touch = TouchKind::GT911;
    c.touch_calibration = true;
    c.beeper = false;
    c.nmea2000_can = false;
    c.sd_card = true;
    return c;
}

bool set_backlight(uint8_t value_0_255) {
    ensure_backlight();
    s_backlight_value = value_0_255;
    // Until we wire a LEDC channel through this API, on/off above 32.
    digitalWrite(BACKLIGHT_PIN, value_0_255 > 32 ? HIGH : LOW);
    return true;
}

uint8_t backlight() { return s_backlight_value; }

bool set_power(bool on) { return set_backlight(on ? 255 : 0); }

}  // namespace board

#endif  // BOARD_ID_SUNTON_4848S040
