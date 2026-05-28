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

// Default-on for the standalone (no board id supplied) build path: if
// nothing else has been selected, fall back to Sunton so existing
// users without an explicit -D in platformio.ini still get a working
// firmware. Once BOARD_ID_DEFINED is set anywhere in the build, this
// file only compiles when the matching id is also set.
#if defined(BOARD_ID_SUNTON_4848S040) || \
    (!defined(BOARD_ID_DEFINED) && !defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4) && !defined(BOARD_ID_NATIVE_FAKE))

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = 38;
constexpr int LEDC_CHANNEL = 0;
constexpr int LEDC_FREQ_HZ = 5000;
constexpr int LEDC_RES_BITS = 8;
uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    // Spec 13 §"Backlight And Power" - own the LEDC channel here so
    // main.cpp doesn't drive the pin directly. Setup is idempotent;
    // a redundant ledcSetup() in main.cpp before this lands is fine
    // (same channel/freq), but the future move is to call
    // board::set_backlight from setup() and remove the main.cpp path.
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(BACKLIGHT_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 255);
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
    ledcWrite(LEDC_CHANNEL, value_0_255);
    return true;
}

uint8_t backlight() { return s_backlight_value; }

bool set_power(bool on) { return set_backlight(on ? 255 : 0); }

}  // namespace board

#endif  // BOARD_ID_SUNTON_4848S040
