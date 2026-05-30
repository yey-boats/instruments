#include "board.h"

#include <driver/ledc.h>
#include <stdint.h>

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
#if defined(BOARD_ID_SUNTON_4848S040) ||                                                           \
    (!defined(BOARD_ID_DEFINED) && !defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4) &&                     \
     !defined(BOARD_ID_NATIVE_FAKE))

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = 38;
constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int LEDC_FREQ_HZ = 5000;
uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    // Spec 13 §"Backlight And Power" - own the LEDC channel here so
    // main.cpp doesn't drive the pin directly. Spec 21 B: moved from
    // Arduino's ledcSetup/ledcAttachPin to the IDF driver so the board
    // layer no longer depends on the Arduino wrapper.
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = BL_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_8_BIT;
    timer_cfg.timer_num = BL_TIMER;
    timer_cfg.freq_hz = LEDC_FREQ_HZ;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = BACKLIGHT_PIN;
    channel_cfg.speed_mode = BL_MODE;
    channel_cfg.channel = BL_CHANNEL;
    channel_cfg.intr_type = LEDC_INTR_DISABLE;
    channel_cfg.timer_sel = BL_TIMER;
    channel_cfg.duty = 255;
    channel_cfg.hpoint = 0;
    ledc_channel_config(&channel_cfg);
    s_backlight_inited = true;
}

}  // namespace

const char *id() {
    return "sunton_4848s040";
}
const char *display_name() {
    return "Sunton/Guition ESP32-4848S040 4.0\" 480x480";
}

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
    ledc_set_duty(BL_MODE, BL_CHANNEL, value_0_255);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
    return true;
}

uint8_t backlight() {
    return s_backlight_value;
}

bool set_power(bool on) {
    return set_backlight(on ? 255 : 0);
}

}  // namespace board

#endif  // BOARD_ID_SUNTON_4848S040
