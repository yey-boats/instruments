// Waveshare ESP32-S3-Touch-LCD-4 board impl. Spec 13 step 10.
//
// Product specs (from the manufacturer's wiki, cross-checked against
// the published schematic):
//   - 4.0" 480x480 IPS, ST7701S RGB parallel panel
//   - GT911 capacitive touch over I2C
//   - PWM backlight (panel-internal pin)
//   - SN65HVD230 CAN transceiver on-board (NMEA2000 capable)
//   - microSD slot
//   - ESP32-S3-WROOM-1 N16R8 (16 MB flash + 8 MB octal PSRAM)
//
// This file ships only the identity + capability flags; full display
// init for this board moves in alongside spec 13 step 3 (display::).
// Until then, the embedded firmware path still wires the panel up in
// main.cpp, and the only difference visible at runtime is what
// board::capabilities() advertises (NMEA2000 + SD enabled).

#if defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4)

#include "board.h"

#include <Arduino.h>

namespace board {

namespace {

// Backlight - the published schematic shows the panel BL net is
// driven by GPIO 5 on the dev board. We don't have hardware to
// confirm yet; the pin is overridable at build-time so the operator
// can correct it without a code change.
#ifndef WAVESHARE_BACKLIGHT_PIN
#define WAVESHARE_BACKLIGHT_PIN 5
#endif
constexpr int BACKLIGHT_PIN = WAVESHARE_BACKLIGHT_PIN;
constexpr int LEDC_CHANNEL = 0;
constexpr int LEDC_FREQ_HZ = 5000;
constexpr int LEDC_RES_BITS = 8;

uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(BACKLIGHT_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 255);
    s_backlight_inited = true;
}

}  // namespace

const char *id() { return "waveshare_touch_lcd_4"; }
const char *display_name() { return "Waveshare ESP32-S3-Touch-LCD-4 4.0\" 480x480"; }

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
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::GT911;
    c.touch_calibration = true;
    c.beeper = false;
    c.nmea2000_can = true;   // dedicated CAN transceiver on-board
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

#endif  // BOARD_ID_WAVESHARE_TOUCH_LCD_4
