#pragma once

// clang-format off

// Waveshare ESP32-S3-Touch-LCD-1.28
// 1.28" 240x240 round IPS, GC9A01 over plain 4-wire SPI (up to 80 MHz),
// CST816S touch, QMI8658 IMU on the same I2C bus. Touch-only round board -
// no rotary encoder (unlike the 1.8" knob).
//
// ESP32-S3R2: 16 MB external flash + 2 MB embedded QUAD PSRAM. The env
// MUST NOT use board_build.arduino.memory_type=qio_opi (that is for the
// octal R8 parts and faults at app entry on this chip) - see platformio.ini.
//
// Pin map sources (NOT yet hardware-verified on our bench):
//   - Waveshare wiki: waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28
//   - github.com/adamcooks/waveshare-esp32s3-touch-lcd-128-platformio
//     (validated community PlatformIO config; matches the wiki on every pin)

#ifndef LCD_W
#define LCD_W   240
#endif
#ifndef LCD_H
#define LCD_H   240
#endif

// GC9A01 over plain SPI.
#define LCD_DC     8
#define LCD_CS     9
#define LCD_SCK    10
#define LCD_MOSI   11
#define LCD_MISO   12
#define LCD_RST    14
#define LCD_BL     2      // backlight, LEDC PWM (active high)

// CST816S capacitive touch (I2C, addr 0x15). Bus shared with QMI8658 IMU.
#define TOUCH_SDA  6
#define TOUCH_SCL  7
#ifndef TOUCH_INT
#define TOUCH_INT  5
#endif
#define TOUCH_RST  13
#ifndef TOUCH_INT_ACTIVE_LOW
#define TOUCH_INT_ACTIVE_LOW 1
#endif

// Battery voltage sense (ADC divider) per the Waveshare wiki.
#define BAT_ADC    1

// clang-format on
