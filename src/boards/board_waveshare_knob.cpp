#include "board.h"

#include <driver/ledc.h>
#include <math.h>
#include <stdint.h>

// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 board::* implementation.
// Round 360x360 ST77916 QSPI. Backlight LEDC PWM on GPIO 47.

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = 47;
constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int LEDC_FREQ_HZ = 5000;
uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
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
    return "waveshare_knob_1_8";
}
const char *display_name() {
    return "Waveshare ESP32-S3-Knob 1.8\" 360x360 round";
}

Geometry geometry() {
    Geometry g{};
    g.width_px = 360;
    g.height_px = 360;
    g.diagonal_tenths_in = 18;
    g.rotation = 0;
    g.square = true;
    g.shape = DisplayShape::Round;
    g.layout_class = LayoutClass::SquareCompact;  // square-aspect; round via shape+usable
    g.density_class = DensityClass::Hdpi;
    // Inscribed square inside the 360 circle: side = 360/sqrt(2) ~= 254,
    // inset = (360-254)/2 ~= 53 each side.
    const uint16_t inset = 53;
    g.usable_x = inset;
    g.usable_y = inset;
    g.usable_width = g.width_px - inset * 2;
    g.usable_height = g.height_px - inset * 2;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::CST816;
    c.touch_calibration = false;
    c.beeper = true;  // routed to DRV2605 haptic
    c.nmea2000_can = false;
    c.sd_card = false;
    c.display_bus = DisplayBus::Qspi;
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

#endif  // BOARD_ID_WAVESHARE_KNOB_1_8
