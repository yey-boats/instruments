// Waveshare ESP32-S3-Touch-LCD profile impl. Spec 13 step 10.
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

#if defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4) || defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4_3) ||        \
    defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4_3B) ||                                                  \
    defined(BOARD_ID_WAVESHARE_TOUCH_LCD_5_800X480) ||                                             \
    defined(BOARD_ID_WAVESHARE_TOUCH_LCD_5_1024X600) ||                                            \
    defined(BOARD_ID_WAVESHARE_TOUCH_LCD_7_800X480) ||                                             \
    defined(BOARD_ID_WAVESHARE_TOUCH_LCD_7B_1024X600)

#include "board.h"

#include <driver/ledc.h>
#include <stdint.h>

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
constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int LEDC_FREQ_HZ = 5000;

uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

struct Profile {
    const char *id;
    const char *name;
    uint16_t width;
    uint16_t height;
    uint16_t diagonal_tenths;
    LayoutClass layout;
    DensityClass density;
};

constexpr Profile kProfile4 = {"waveshare_touch_lcd_4",
                               "Waveshare ESP32-S3-Touch-LCD-4 4.0\" 480x480",
                               480,
                               480,
                               40,
                               LayoutClass::SquareCompact,
                               DensityClass::Mdpi};
constexpr Profile kProfile43 = {"waveshare_touch_lcd_4_3",
                                "Waveshare ESP32-S3-Touch-LCD-4.3 4.3\" 800x480",
                                800,
                                480,
                                43,
                                LayoutClass::LandscapeCompact,
                                DensityClass::Mdpi};
constexpr Profile kProfile43b = {"waveshare_touch_lcd_4_3b",
                                 "Waveshare ESP32-S3-Touch-LCD-4.3B 4.3\" 800x480",
                                 800,
                                 480,
                                 43,
                                 LayoutClass::LandscapeCompact,
                                 DensityClass::Mdpi};
constexpr Profile kProfile5_800 = {"waveshare_touch_lcd_5_800x480",
                                   "Waveshare ESP32-S3-Touch-LCD-5 5.0\" 800x480",
                                   800,
                                   480,
                                   50,
                                   LayoutClass::LandscapeCompact,
                                   DensityClass::Mdpi};
constexpr Profile kProfile5_1024 = {"waveshare_touch_lcd_5_1024x600",
                                    "Waveshare ESP32-S3-Touch-LCD-5 5.0\" 1024x600",
                                    1024,
                                    600,
                                    50,
                                    LayoutClass::LandscapeWide,
                                    DensityClass::Hdpi};
constexpr Profile kProfile7_800 = {"waveshare_touch_lcd_7_800x480",
                                   "Waveshare ESP32-S3-Touch-LCD-7 7.0\" 800x480",
                                   800,
                                   480,
                                   70,
                                   LayoutClass::LandscapeCompact,
                                   DensityClass::Mdpi};
constexpr Profile kProfile7b = {"waveshare_touch_lcd_7b_1024x600",
                                "Waveshare ESP32-S3-Touch-LCD-7B 7.0\" 1024x600",
                                1024,
                                600,
                                70,
                                LayoutClass::LandscapeWide,
                                DensityClass::Hdpi};

const Profile &profile() {
#if defined(BOARD_ID_WAVESHARE_TOUCH_LCD_7B_1024X600)
    return kProfile7b;
#elif defined(BOARD_ID_WAVESHARE_TOUCH_LCD_7_800X480)
    return kProfile7_800;
#elif defined(BOARD_ID_WAVESHARE_TOUCH_LCD_5_1024X600)
    return kProfile5_1024;
#elif defined(BOARD_ID_WAVESHARE_TOUCH_LCD_5_800X480)
    return kProfile5_800;
#elif defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4_3B)
    return kProfile43b;
#elif defined(BOARD_ID_WAVESHARE_TOUCH_LCD_4_3)
    return kProfile43;
#else
    return kProfile4;
#endif
}

void ensure_backlight() {
    if (s_backlight_inited) return;
    // Spec 21 B: IDF LEDC driver (no Arduino wrapper).
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
    return profile().id;
}
const char *display_name() {
    return profile().name;
}

Geometry geometry() {
    const Profile &p = profile();
    Geometry g{};
    g.width_px = p.width;
    g.height_px = p.height;
    g.diagonal_tenths_in = p.diagonal_tenths;
    g.rotation = 0;
    g.square = p.width == p.height;
    g.shape = g.square ? DisplayShape::Square : DisplayShape::Rectangle;
    g.layout_class = p.layout;
    g.density_class = p.density;
    g.usable_x = 0;
    g.usable_y = 0;
    g.usable_width = p.width;
    g.usable_height = p.height;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::GT911;
    c.touch_calibration = true;
    c.beeper = false;
    c.nmea2000_can = true;  // dedicated CAN transceiver on-board
    c.sd_card = true;
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

#endif  // BOARD_ID_WAVESHARE_TOUCH_LCD_4
