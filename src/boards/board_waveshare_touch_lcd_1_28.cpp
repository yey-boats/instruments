// Waveshare ESP32-S3-Touch-LCD-1.28 board::* implementation.
// Round 1.28" 240x240 GC9A01 over plain SPI, CST816S touch, LEDC PWM
// backlight. Touch-only (no rotary encoder - unlike the 1.8" knob).
// Pin map in include/board_pins_waveshare_touch_lcd_1_28.h.
//
// PSRAM note: this board's module is an ESP32-S3R2 (2 MB QUAD PSRAM),
// not the N16R8 octal part the other boards use. The env must NOT set
// board_build.arduino.memory_type=qio_opi (see platformio.ini).

#if defined(BOARD_ID_WAVESHARE_TOUCH_LCD_1_28)

#include "board.h"

#include <driver/ledc.h>
#include <stdint.h>

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = LCD_BL;
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
    return "waveshare_touch_lcd_1_28";
}
const char *display_name() {
    return "Waveshare ESP32-S3-Touch-LCD-1.28 240x240 round";
}

Geometry geometry() {
    Geometry g{};
    g.width_px = 240;
    g.height_px = 240;
    g.diagonal_tenths_in = 13;  // 1.28"
    g.rotation = 0;
    g.square = true;
    // Same convention as the 1.8" knob: round is shape + usable-area
    // inset, layout class stays SquareCompact (aspect-driven).
    g.shape = DisplayShape::Round;
    g.layout_class = LayoutClass::SquareCompact;
    g.density_class = DensityClass::Mdpi;  // 240 px / 1.28" ~= 187 ppi
    // Inscribed square inside the 240 circle: side = 240/sqrt(2) ~= 170,
    // inset = (240-170)/2 = 35 each side.
    const uint16_t inset = 35;
    g.usable_x = inset;
    g.usable_y = inset;
    g.usable_width = g.width_px - inset * 2;
    g.usable_height = g.height_px - inset * 2;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;  // 2 MB quad PSRAM (LVGL pool + layout Config)
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::CST816;
    c.touch_calibration = false;
    c.beeper = false;
    c.nmea2000_can = false;
    c.sd_card = false;
    c.display_bus = DisplayBus::Spi;
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

#endif  // BOARD_ID_WAVESHARE_TOUCH_LCD_1_28
