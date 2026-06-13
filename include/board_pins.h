#pragma once

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include "board_pins_waveshare_knob.h"
#else
// clang-format off

// Sunton / Guition ESP32-4848S040 (also labeled ESP32-4840S040)
// 4.0" 480x480 IPS, ST7701S RGB parallel panel
// ESP32-S3-WROOM-1 N16R8 (16 MB flash + 8 MB octal PSRAM)

#ifndef LCD_W
#define LCD_W   480
#endif
#ifndef LCD_H
#define LCD_H   480
#endif

// ST7701 init via 3-wire SPI (used only at boot, then RGB takes over)
#define ST7701_CS    39
#define ST7701_SCK   48
#define ST7701_MOSI  47

// RGB parallel panel pins
#define RGB_DE       18
#define RGB_VSYNC    17
#define RGB_HSYNC    16
#define RGB_PCLK     21

// R0..R4, G0..G5, B0..B4 - verified against aquaElectronics/esp32-4848s040-st7701
#define RGB_R0  11
#define RGB_R1  12
#define RGB_R2  13
#define RGB_R3  14
#define RGB_R4  0
#define RGB_G0  8
#define RGB_G1  20
#define RGB_G2  3
#define RGB_G3  46
#define RGB_G4  9
#define RGB_G5  10
#define RGB_B0  4
#define RGB_B1  5
#define RGB_B2  6
#define RGB_B3  7
#define RGB_B4  15

#define LCD_BL  38     // backlight (active high)

// GT911 capacitive touch (I2C). INT/RST typically unconnected on this board.
// Set TOUCH_INT to a GPIO on boards that route GT911 INT; firmware will then
// wait for touch interrupts while idle and fall back to timed reads while a
// contact is active.
#define TOUCH_SDA  19
#define TOUCH_SCL  45
#ifndef TOUCH_INT
#define TOUCH_INT  -1
#endif
#define TOUCH_RST  -1
#ifndef TOUCH_INT_ACTIVE_LOW
#define TOUCH_INT_ACTIVE_LOW 1
#endif

// MicroSD (SPI mode)
#define SD_CS   42

// Candidates for the 3 relay lines on the daughterboard (mapping TBD).
// Note: most user GPIOs on this board are consumed by the RGB panel.
// Likely relay-header GPIOs reuse the SD bank or unused panel pins.
#define RELAY_CANDIDATE_GPIOS_HINT "consumed by RGB; needs board-specific check"

// clang-format on
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8 / else
