// Sunton/Guition ESP32-8048S050 (5.0") and ESP32-8048S070 (7.0") board::*
// implementation. Both are ESP32-S3-N16R8 (16 MB flash + 8 MB octal PSRAM)
// with an 800x480 RGB parallel TTL panel (ST7262/EK9716-class controller:
// no init-command table, the panel is driven directly by RGB timing) and
// GT911 capacitive touch. Pin maps live in
// include/board_pins_sunton_8048s050.h / _8048s070.h.
//
// Same first-phase scope as the other board impls: identity, geometry,
// capabilities, backlight. Display/touch init stays in main.cpp until
// spec 13 step 3 (display::) lands.
//
// NOT hardware-verified yet: built from community pin maps (sources cited
// in the pins headers). Backlight is GPIO 2 on both boards per those maps;
// overridable at build time until confirmed on hardware.

#if defined(BOARD_ID_SUNTON_8048S050) || defined(BOARD_ID_SUNTON_8048S070)

#include "board.h"

#include <driver/ledc.h>
#include <stdint.h>

namespace board {

namespace {

#ifndef SUNTON_8048_BACKLIGHT_PIN
#define SUNTON_8048_BACKLIGHT_PIN 2
#endif
constexpr int BACKLIGHT_PIN = SUNTON_8048_BACKLIGHT_PIN;
constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int LEDC_FREQ_HZ = 5000;

uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    // Spec 21 B: IDF LEDC driver (no Arduino wrapper), same as the other
    // board impls so LEDC ownership stays in the board layer.
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
#if defined(BOARD_ID_SUNTON_8048S070)
    return "sunton_8048s070";
#else
    return "sunton_8048s050";
#endif
}

const char *display_name() {
#if defined(BOARD_ID_SUNTON_8048S070)
    return "Sunton/Guition ESP32-8048S070 7.0\" 800x480";
#else
    return "Sunton/Guition ESP32-8048S050 5.0\" 800x480";
#endif
}

Geometry geometry() {
    Geometry g{};
    g.width_px = 800;
    g.height_px = 480;
#if defined(BOARD_ID_SUNTON_8048S070)
    g.diagonal_tenths_in = 70;
#else
    g.diagonal_tenths_in = 50;
#endif
    g.rotation = 0;
    g.square = false;
    g.shape = DisplayShape::Rectangle;
    // Same class the Waveshare 5"/7" 800x480 profiles use: 800x480 stays
    // LandscapeCompact; ui::layout_context() still reports wide=true for
    // the 7" via the diagonal_tenths_in >= 70 rule.
    g.layout_class = LayoutClass::LandscapeCompact;
    g.density_class = DensityClass::Mdpi;
    g.usable_x = 0;
    g.usable_y = 0;
    g.usable_width = g.width_px;
    g.usable_height = g.height_px;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::GT911;
    c.touch_calibration = true;
    c.beeper = false;
    c.nmea2000_can = false;
    c.sd_card = true;  // microSD on shared SPI (see pins header)
    c.display_bus = DisplayBus::RgbParallel;
    c.touch_interrupt = TOUCH_INT >= 0;
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

#endif  // BOARD_ID_SUNTON_8048S050 || BOARD_ID_SUNTON_8048S070
