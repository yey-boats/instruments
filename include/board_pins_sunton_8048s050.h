#pragma once

// clang-format off

// Sunton/Guition ESP32-8048S050
// 5.0" 800x480 IPS, ST7262 RGB parallel TTL panel (no init-command table:
// the controller is configured by strapping, the panel is driven directly
// by RGB timing), GT911 capacitive touch.
// ESP32-S3-WROOM-1 N16R8 (16 MB flash + 8 MB OCTAL PSRAM -> qio_opi).
//
// Pin map sources (NOT yet hardware-verified on our bench):
//   - Arduino_GFX dev-device ESP32_8048S043 block (S043/S050 share the map):
//     github.com/moononournation/Arduino_GFX PDQgraphicstest/Arduino_GFX_dev_device.h
//   - github.com/rzeldent/platformio-espressif32-sunton esp32-8048S050C.json
//     (same GPIOs; note rzeldent labels the R and B bundles OPPOSITELY -
//     naming convention of its esp_lcd driver, not different wiring)
//   - ESPHome community configs (clowrey/esphome-esp32-8048s050-lvgl)
//
// WARNING (CLAUDE.md R/B trap): R0..R4 vs B0..B4 labeling disagrees between
// references for ALL Sunton boards. The map below uses the Arduino_GFX
// convention (R0=45), consistent with the Arduino_GFX panel class we build
// with. VERIFY WITH A CAMERA COLOR TEST on hardware before trusting red vs
// blue; if swapped, exchange the two 5-pin groups.

#ifndef LCD_W
#define LCD_W   800
#endif
#ifndef LCD_H
#define LCD_H   480
#endif

// RGB parallel panel pins
#define RGB_DE       40
#define RGB_VSYNC    41
#define RGB_HSYNC    39
#define RGB_PCLK     42

#define RGB_R0  45
#define RGB_R1  48
#define RGB_R2  47
#define RGB_R3  21
#define RGB_R4  14
#define RGB_G0  5
#define RGB_G1  6
#define RGB_G2  7
#define RGB_G3  15
#define RGB_G4  16
#define RGB_G5  4
#define RGB_B0  8
#define RGB_B1  3
#define RGB_B2  46
#define RGB_B3  9
#define RGB_B4  1

// Panel timing (Arduino_GFX ESP32_8048S043 values, matched by rzeldent's
// S050C JSON). ESPHome configs run the pixel clock at 12.5-14 MHz; 16 MHz
// is the canonical Arduino_GFX value - drop it if flicker/tearing shows.
#define RGB_HSYNC_POLARITY    0
#define RGB_HSYNC_FRONT_PORCH 8
#define RGB_HSYNC_PULSE_WIDTH 4
#define RGB_HSYNC_BACK_PORCH  8
#define RGB_VSYNC_POLARITY    0
#define RGB_VSYNC_FRONT_PORCH 8
#define RGB_VSYNC_PULSE_WIDTH 4
#define RGB_VSYNC_BACK_PORCH  8
#define RGB_PCLK_ACTIVE_NEG   1
#ifndef RGB_PCLK_HZ
#define RGB_PCLK_HZ           16000000L
#endif

#define LCD_BL  2      // backlight (LEDC PWM, active high)

// GT911 capacitive touch (I2C addr 0x5D; firmware probes 0x5D then 0x14).
// INT is routed on this board (GPIO 18, per rzeldent S050C JSON); poll
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
