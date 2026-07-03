#pragma once

// clang-format off

// Sunton/Guition ESP32-8048S070
// 7.0" 800x480 IPS, EK9716BD3 + EK73002ACGB RGB parallel TTL panel (no
// init-command table: integrated timing controller configured by strapping,
// the panel is driven directly by RGB timing), GT911 capacitive touch.
// ESP32-S3-WROOM-1 N16R8 (16 MB flash + 8 MB OCTAL PSRAM -> qio_opi).
//
// Pin map sources (NOT yet hardware-verified on our bench):
//   - Arduino_GFX dev-device ESP32_8048S070 block:
//     github.com/moononournation/Arduino_GFX PDQgraphicstest/Arduino_GFX_dev_device.h
//   - github.com/rzeldent/platformio-espressif32-sunton esp32-8048S070C.json
//     (same GPIOs; rzeldent labels the R and B bundles OPPOSITELY - naming
//     convention of its esp_lcd driver, not different wiring)
//
// NOTE vs the 5" 8048S050: DE and VSYNC are SWAPPED on this board
// (S070: DE=41 VSYNC=40; S050: DE=40 VSYNC=41). Both references agree.
//
// WARNING (CLAUDE.md R/B trap): R0..R4 vs B0..B4 labeling disagrees between
// references for ALL Sunton boards. The map below uses the Arduino_GFX
// convention (R0=14), consistent with the Arduino_GFX panel class we build
// with. VERIFY WITH A CAMERA COLOR TEST on hardware before trusting red vs
// blue; if swapped, exchange the two 5-pin groups.

#ifndef LCD_W
#define LCD_W   800
#endif
#ifndef LCD_H
#define LCD_H   480
#endif

// RGB parallel panel pins
#define RGB_DE       41
#define RGB_VSYNC    40
#define RGB_HSYNC    39
#define RGB_PCLK     42

#define RGB_R0  14
#define RGB_R1  21
#define RGB_R2  47
#define RGB_R3  48
#define RGB_R4  45
#define RGB_G0  9
#define RGB_G1  46
#define RGB_G2  3
#define RGB_G3  8
#define RGB_G4  16
#define RGB_G5  1
#define RGB_B0  15
#define RGB_B1  7
#define RGB_B2  6
#define RGB_B3  5
#define RGB_B4  4

// Panel timing (Arduino_GFX ESP32_8048S070 values). rzeldent's S070C JSON
// differs slightly (front porches 210/22, pclk 12.5 MHz) - both are known
// working; these are the proven values for the Arduino_GFX RGB class.
#define RGB_HSYNC_POLARITY    0
#define RGB_HSYNC_FRONT_PORCH 180
#define RGB_HSYNC_PULSE_WIDTH 30
#define RGB_HSYNC_BACK_PORCH  16
#define RGB_VSYNC_POLARITY    0
#define RGB_VSYNC_FRONT_PORCH 12
#define RGB_VSYNC_PULSE_WIDTH 13
#define RGB_VSYNC_BACK_PORCH  10
#define RGB_PCLK_ACTIVE_NEG   1
#ifndef RGB_PCLK_HZ
#define RGB_PCLK_HZ           12000000L
#endif

#define LCD_BL  2      // backlight (LEDC PWM, active high)

// GT911 capacitive touch (I2C addr 0x5D; firmware probes 0x5D then 0x14).
// INT is routed on this board (GPIO 18, per rzeldent S070C JSON); poll
// fallback is available with -D TOUCH_INT=-1 or the runtime `touch.mode`
// command if IRQ mode misbehaves on real hardware.
#define TOUCH_SDA  19
#define TOUCH_SCL  20
#ifndef TOUCH_INT
#define TOUCH_INT  18
#endif
#define TOUCH_RST  38
#ifndef TOUCH_INT_ACTIVE_LOW
#define TOUCH_INT_ACTIVE_LOW 1
#endif

// MicroSD (SPI mode): CS=10, MOSI=11, SCLK=12, MISO=13.
#define SD_CS   10

// clang-format on
