#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include <CST816S.h>
#endif

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
#ifdef RENDER_DOUBLE_BUFFER
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#endif
#ifdef DBG_PERF_COUNTERS
#include "bench_row.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include "layout_loader.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "screens.h"
#include "web.h"
#include "screenshot.h"
#include "app_events.h"
#include "touch_cal.h"
#include "config_runtime.h"
#include "latency.h"
#include "source_nmea_wifi.h"
#include "source_nmea2000.h"
#include "device_identity.h"
#include "manager.h"
#include "beeper.h"
#include "autopilot.h"
#include "board.h"
#include "build_config.h"
#include "proto_target.h"
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include "knob_ui.h"
#include "knob_input.h"
#endif

#include "storage.h"
#include "midl_demo_doc.h"
#include "midl_render.h"
#include "psram_json.h"
#include <math.h>
#include <string.h>

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Waveshare ESP32-S3-Knob 1.8" 360x360 round panel: ST77916 over Quad-SPI.
// CS/SCK + D0..D3 from the knob pin map (include/board_pins_waveshare_knob.h).
// Arduino_ESP32QSPI ctor is (cs, sck, mosi=D0, miso=D1, quadwp=D2, quadhd=D3).
static Arduino_DataBus *bus =
    new Arduino_ESP32QSPI(QSPI_CS, QSPI_SCK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
// gfx is the Arduino_GFX base pointer; the LVGL flush path uses
// gfx->draw16bitRGBBitmap (a base virtual) so it stays board-agnostic.
static Arduino_GFX *gfx =
    new Arduino_ST77916(bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

#else
// ST7701 init via 3-wire SPI, then RGB takes over.
static Arduino_DataBus *bus =
    new Arduino_SWSPI(-1 /* DC */, ST7701_CS, ST7701_SCK, ST7701_MOSI, -1 /* MISO */);

// Custom ST7701 init for Sunton/Guition ESP32-4848S040 panel
// (verified working against aquaElectronics/esp32-4848s040-st7701).
// clang-format off
static const uint8_t st7701_4848S040_init[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8,  0xCD, 0x00,
    WRITE_COMMAND_8, 0xB0,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,
    WRITE_COMMAND_8, 0xB1,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
    WRITE_C8_D8, 0xB0, 0x60,
    WRITE_C8_D8, 0xB1, 0x32,
    WRITE_C8_D8, 0xB2, 0x07,
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,
    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x1B, 0x02,
    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44,
    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00,
    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,
    WRITE_C8_D16, 0xE4, 0x44, 0x44,
    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
    0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,
    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,
    WRITE_C8_D16, 0xE7, 0x44, 0x44,
    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
    0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,
    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40,
    WRITE_C8_D16, 0xEC, 0x3C, 0x00,
    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
    WRITE_C8_D8, 0xE5, 0xE4,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,
    WRITE_C8_D8, 0x3A, 0x60,  // RGB666 - matches working firmware
    DELAY, 10,
    WRITE_COMMAND_8, 0x11,    // sleep out
    END_WRITE,
    DELAY, 120,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,    // display on
    END_WRITE
};
// clang-format on

// Arduino_GFX's RGB panel classes (Arduino_ESP32RGBPanel / Arduino_RGB_Display)
// exist only in the library's ESP_ARDUINO_VERSION_MAJOR<3 branch, so on the
// IDF-5 / Arduino-3.x build (RENDER_DOUBLE_BUFFER) they don't compile. There we
// drive the RGB panel directly via esp_lcd (display_db_init, num_fbs=2). `bus`
// (Arduino_SWSPI, above) survives on Arduino 3.x and still carries the ST7701
// 3-wire SPI init in both paths.
#ifndef RENDER_DOUBLE_BUFFER
static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK, RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4, RGB_G0, RGB_G1,
    RGB_G2, RGB_G3, RGB_G4, RGB_G5, RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4, 1 /* hsync_polarity */,
    10, 8, 50, 1 /* vsync_polarity */, 10, 8, 20, 1 /* pclk_active_neg */,
    12000000L /* prefer_speed 12MHz */, false /* useBigEndian */);

static Arduino_RGB_Display *gfx =
    new Arduino_RGB_Display(LCD_W, LCD_H, rgbpanel, 0 /* rotation */, true /* auto_flush */, bus,
                            -1 /* RST */, st7701_4848S040_init, sizeof(st7701_4848S040_init));
#endif  // !RENDER_DOUBLE_BUFFER
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8

static bool touch_present = false;

// FPS / render benchmark counters (updated from disp_flush_cb).
// Per docs/specs/09: track flushed pixel volume too so we can compare
// before/after invalidation tweaks.
static volatile uint32_t g_flush_count = 0;
static volatile uint32_t g_flush_us_total = 0;
static volatile uint32_t g_flush_us_peak = 0;
static volatile uint64_t g_flush_px_total = 0;
static volatile uint32_t g_flush_px_peak = 0;  // largest single rect (W*H)

// Latency stamps consumed by latency::record():
//   g_last_invalidate_us : set by ui_refresh when it forces a full-
//                          screen invalidate; cleared after first flush
//                          that follows. The delta is "render latency".
//   g_last_flush_us      : timestamp of the previous flush, for the
//                          frame-interval channel.
static volatile uint32_t g_last_invalidate_us = 0;
static volatile uint32_t g_last_flush_us = 0;

// bench-sweep "first load" latency: the sweep arms this right before ui::show()
// and stamps g_ff_show_us; the next disp_flush_cb records show->first-flush us
// into g_ff_us and disarms. Always compiled (disp_flush_cb references it); only
// armed while a sweep is running.
static volatile bool g_ff_arm = false;
static volatile uint32_t g_ff_show_us = 0;
static volatile uint32_t g_ff_us = 0;

// DIRECT render path: when non-null, LVGL renders straight into the RGB panel's
// PSRAM scanout framebuffer, so disp_flush_cb only writes back the dirty rows
// (cache coherency for the LCD DMA) instead of CPU-copying every tile. Set in
// setup() from gfx->getFramebuffer(). Cache_WriteBack_Addr (declared by the GFX
// panel header) is the S3 ROM cache op the library itself uses after FB writes.
static uint16_t *g_direct_fb = nullptr;

#ifdef RENDER_DOUBLE_BUFFER
// Double-buffered RGB panel via esp_lcd (IDF 5, num_fbs=2). LVGL renders into
// the back framebuffer; disp_flush_cb presents the whole fb with
// esp_lcd_panel_draw_bitmap, which swaps the scanout to it on the next vsync ->
// the scanout always shows a COMPLETE frame (no flicker) and the swap is
// zero-copy (no blit). esp_lcd handles PSRAM cache coherency internally.
static esp_lcd_panel_handle_t g_db_panel = nullptr;
static uint16_t *g_db_fb0 = nullptr;
static uint16_t *g_db_fb1 = nullptr;

// ST7701 register init over the existing 3-wire SPI bus (Arduino_SWSPI survives
// on Arduino 3.x), then a 2-framebuffer RGB panel with the exact timings / pins
// / data-line order of the verified Arduino_GFX panel (board_pins.h; the RGB565
// data order is the verified map - do NOT reorder, see CLAUDE.md R/B trap).
// Returns false on any esp_lcd error.
static bool display_db_init() {
    bus->begin();
    bus->batchOperation((uint8_t *)st7701_4848S040_init, sizeof(st7701_4848S040_init));

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz = 12000000;
    cfg.timings.h_res = LCD_W;
    cfg.timings.v_res = LCD_H;
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 50;
    cfg.timings.hsync_front_porch = 10;
    cfg.timings.vsync_pulse_width = 8;
    cfg.timings.vsync_back_porch = 20;
    cfg.timings.vsync_front_porch = 10;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    // Two framebuffers + LVGL DIRECT mode (the two FBs ARE LVGL's draw buffers).
    // LVGL renders dirty areas into the off-screen FB and keeps both FBs in sync;
    // disp_flush_cb flips scan-out to the just-rendered FB on the frame's last
    // area. rgb_panel_draw_bitmap detects the source == one of our FBs and does a
    // ZERO-COPY swap (repoints the DMA links + writes the FB back from cache), so
    // the scanout always shows a COMPLETE frame (no flicker) and there's no blit.
    // The hybrid rebuilds esp_lcd from source IDF, so unlike the precompiled
    // Arduino esp_lcd this honours the timings/flags below.
    cfg.num_fbs = 2;
    // bounce buffers (cfg.bounce_buffer_size_px) — the textbook fix for the
    // RGB-DMA-vs-flash-write cache panic — are deliberately LEFT OFF in this
    // batch. They need the refill ISR built IRAM-safe (CONFIG_LCD_RGB_ISR_IRAM_SAFE,
    // now set in sdkconfig.defaults under the hybrid), but enabling them is a
    // separate variable from the double-buffer flip; turning both on at once would
    // make a panic/no-panic result ambiguous. Bounce buffers + OTA-while-rendering
    // soak are a deliberate follow-up (spec 21 §H driver #1).
    cfg.psram_trans_align = 64;
    cfg.hsync_gpio_num = RGB_HSYNC;
    cfg.vsync_gpio_num = RGB_VSYNC;
    cfg.de_gpio_num = RGB_DE;
    cfg.pclk_gpio_num = RGB_PCLK;
    cfg.disp_gpio_num = -1;
    const int dg[16] = {RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4, RGB_G0, RGB_G1, RGB_G2,
                        RGB_G3, RGB_G4, RGB_G5, RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4};
    for (int i = 0; i < 16; ++i)
        cfg.data_gpio_nums[i] = dg[i];
    cfg.flags.fb_in_psram = 1;

    if (esp_lcd_new_rgb_panel(&cfg, &g_db_panel) != ESP_OK) return false;
    if (esp_lcd_panel_reset(g_db_panel) != ESP_OK) return false;
    if (esp_lcd_panel_init(g_db_panel) != ESP_OK) return false;
    if (esp_lcd_rgb_panel_get_frame_buffer(g_db_panel, 2, (void **)&g_db_fb0,
                                           (void **)&g_db_fb1) != ESP_OK)
        return false;
    memset(g_db_fb0, 0, (size_t)LCD_W * LCD_H * sizeof(uint16_t));
    memset(g_db_fb1, 0, (size_t)LCD_W * LCD_H * sizeof(uint16_t));
    return true;
}
#endif  // RENDER_DOUBLE_BUFFER

static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
    uint32_t t0 = micros();
    if (g_ff_arm) {  // first flush after a bench-sweep screen-show: record load latency
        g_ff_us = t0 - g_ff_show_us;
        g_ff_arm = false;
    }
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    uint32_t px = w * h;
#ifdef RENDER_DOUBLE_BUFFER
    // DIRECT mode, num_fbs=2: px_map is the BASE of the panel framebuffer LVGL
    // rendered this frame into (buf_1/buf_2 == g_db_fb0/g_db_fb1; in DIRECT mode
    // LVGL passes the buffer base, not the tile offset). LVGL renders every dirty
    // area into the same FB, then flushes each area; only on the LAST area do we
    // flip scan-out to that whole FB. rgb_panel_draw_bitmap sees the source ==
    // one of our FBs -> zero-copy swap (repoints DMA links + one full-frame cache
    // writeback). LVGL then renders the next frame into the OTHER FB (it alternates
    // buf_act and keeps both FBs in sync), so we never draw into the FB being
    // scanned out -> tear-free, no blit. (Flip only on is_last to avoid a
    // full-frame cache msync per dirty tile.)
    if (lv_display_flush_is_last(d)) {
        esp_lcd_panel_draw_bitmap(g_db_panel, 0, 0, LCD_W, LCD_H, px_map);
    }
#else
    if (g_direct_fb) {
        // LVGL already rendered this area into the scanout framebuffer; just
        // write back the dirty rows so the RGB DMA sees them. No pixel copy.
        Cache_WriteBack_Addr((uint32_t)(g_direct_fb + (size_t)area->y1 * LCD_W),
                             (uint32_t)(LCD_W * h * sizeof(uint16_t)));
    } else {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    }
#endif
    lv_display_flush_ready(d);
    uint32_t dt = micros() - t0;
    g_flush_count++;
    g_flush_us_total += dt;
    g_flush_px_total += px;
    if (dt > g_flush_us_peak) g_flush_us_peak = dt;
    if (px > g_flush_px_peak) g_flush_px_peak = px;

    // Latency channels.
    if (g_last_flush_us) {
        latency::record(latency::Channel::FrameInterval, t0 - g_last_flush_us);
    }
    g_last_flush_us = t0;
    if (g_last_invalidate_us) {
        latency::record(latency::Channel::RenderLatency, t0 - g_last_invalidate_us);
        g_last_invalidate_us = 0;  // only the first flush after invalidate counts
    }
}

// ----- Touch task --------------------------------------------------------
// I2C reads from the LVGL indev callback could stall the render task. Move
// polling to a dedicated FreeRTOS task on core 0 (LVGL stays on core 1).
// The task posts the latest point into a shared snapshot guarded by a
// mutex; the indev callback just copies the snapshot - never touches I2C.
// 16 ms poll period = ~60 Hz sample rate, matches the panel refresh.

struct TouchSnapshot {
    int16_t x;
    int16_t y;
    bool pressed;
    uint32_t last_ms;
    int16_t raw_x;  // pre-calibration, for the calibration screen
    int16_t raw_y;
};
static TouchSnapshot g_touch = {-1, -1, false, 0, -1, -1};
static SemaphoreHandle_t g_touch_mtx = nullptr;
static SemaphoreHandle_t g_touch_i2c_mtx = nullptr;
static TaskHandle_t g_touch_task = nullptr;
static TaskHandle_t g_lvgl_task = nullptr;  // dedicated LVGL pump (core 1)
static void lvgl_task(void *);              // defined near loop()

static uint8_t g_gt911_addr = 0x5D;
static volatile uint32_t g_i2c_err_count = 0;
static volatile uint32_t g_i2c_ok_count = 0;
static volatile uint32_t g_gt_ready_count = 0;   // status had 0x80 set
static volatile uint32_t g_gt_points_count = 0;  // status reported >=1 point
static volatile uint32_t g_touch_irq_count = 0;
static bool g_touch_irq_enabled = false;
extern "C" {
uint32_t main_i2c_err_count() {
    return g_i2c_err_count;
}
uint32_t main_i2c_ok_count() {
    return g_i2c_ok_count;
}
uint32_t main_gt_ready_count() {
    return g_gt_ready_count;
}
uint32_t main_gt_points_count() {
    return g_gt_points_count;
}
uint32_t main_touch_irq_count() {
    return g_touch_irq_count;
}
const char *main_touch_mode() {
    return g_touch_irq_enabled ? "irq" : "poll";
}

// Spec 17 §8 touch.mode runtime switch. Returns true on a successful
// transition (including "already there"). "irq" requires both a valid
// TOUCH_INT pin AND the touch task running so the ISR has somewhere to
// notify; otherwise it returns false and the caller surfaces
// `invalid_payload` / `failed`. "poll" is always accepted.
bool main_set_touch_mode(const char *mode);
}

static void IRAM_ATTR touch_irq_isr() {
    g_touch_irq_count++;
    BaseType_t hp_task_woken = pdFALSE;
    if (g_touch_task) {
        vTaskNotifyGiveFromISR(g_touch_task, &hp_task_woken);
        if (hp_task_woken) portYIELD_FROM_ISR();
    }
}

static bool gt911_read_regs_locked(uint16_t reg, uint8_t *buf, size_t len) {
    if (!buf || len == 0) return false;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > 32) n = 32;
        Wire.beginTransmission(g_gt911_addr);
        Wire.write((uint8_t)(reg >> 8));
        Wire.write((uint8_t)(reg & 0xFF));
        if (Wire.endTransmission(false) != 0) {
            g_i2c_err_count++;
            return false;
        }
        size_t got = Wire.requestFrom((int)g_gt911_addr, (int)n);
        if (got != n) {
            g_i2c_err_count++;
            return false;
        }
        for (size_t i = 0; i < n; ++i)
            buf[off + i] = Wire.read();
        g_i2c_ok_count++;
        off += n;
        reg += n;
    }
    return true;
}

static bool gt911_write_reg_locked(uint16_t reg, uint8_t value) {
    Wire.beginTransmission(g_gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(value);
    if (Wire.endTransmission() != 0) {
        g_i2c_err_count++;
        return false;
    }
    g_i2c_ok_count++;
    return true;
}

// ----- GT911 INT pin probe utility ---------------------------------------
// docs/specs/14-touch-interrupt-testing.md says the GT911 INT line is not
// known to be routed on the Sunton/Guition 4848S040. The probe arms each
// candidate GPIO with INPUT_PULLUP + a counted FALLING interrupt; the
// user touches the panel; we dump the counters. Whichever pin's counter
// climbs is the routed INT line (if any).
//
// Candidate set: ESP32-S3 GPIOs that are
//   - not used by RGB panel, I2C, ST7701 SPI, SD, backlight
//   - not internal to PSRAM/flash on the N16R8 module (33-37 reserved)
//   - NOT UART0 TX/RX (43/44) - those carry the CH340 console; touching
//     them locks the serial debug channel.
// Safe candidates: GPIO 1, 2, 40, 41.
//
// Console commands:
//   irq-probe        - arm probes, sample for 10 seconds, auto-disarm
//   irq-probe-dump   - print current counts without disarming
namespace irq_probe {
static const int s_pins[] = {1, 2, 40, 41};
static constexpr int N = sizeof(s_pins) / sizeof(s_pins[0]);
static volatile uint32_t s_counts[N] = {0};
static bool s_armed = false;
static uint32_t s_arm_ms = 0;

static void IRAM_ATTR isr(void *arg) {
    int idx = (int)(intptr_t)arg;
    if (idx >= 0 && idx < N) s_counts[idx]++;
}

static void dump() {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[irq-probe] %s elapsed=%lums ", s_armed ? "armed" : "off",
                     (unsigned long)(s_arm_ms ? (millis() - s_arm_ms) : 0));
    for (int i = 0; i < N && n < (int)sizeof(buf) - 16; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "g%d=%lu ", s_pins[i], (unsigned long)s_counts[i]);
    }
    net::logf("%s", buf);
}

static void disarm() {
    if (!s_armed) return;
    for (int i = 0; i < N; ++i) {
        detachInterrupt(digitalPinToInterrupt(s_pins[i]));
    }
    s_armed = false;
    net::logf("[irq-probe] disarmed");
    dump();
}

static void arm() {
    if (s_armed) return;
    for (int i = 0; i < N; ++i) {
        s_counts[i] = 0;
        pinMode(s_pins[i], INPUT_PULLUP);
        attachInterruptArg(digitalPinToInterrupt(s_pins[i]), isr, (void *)(intptr_t)i, FALLING);
    }
    s_armed = true;
    s_arm_ms = millis();
    net::logf("[irq-probe] armed gpios=%d active_low FALLING - "
              "touch the panel; auto-disarms in 10s",
              N);
}

static void tick() {
    if (!s_armed) return;
    if (millis() - s_arm_ms >= 10000)
        disarm();
    else
        dump();
}
}  // namespace irq_probe

static bool gt911_read_once(int16_t *out_x, int16_t *out_y) {
    bool locked = true;
    if (g_touch_i2c_mtx) {
        locked = xSemaphoreTake(g_touch_i2c_mtx, pdMS_TO_TICKS(20)) == pdTRUE;
    }
    if (!locked) return false;

    uint8_t status = 0;
    if (!gt911_read_regs_locked(0x814E, &status, 1)) {
        if (g_touch_i2c_mtx) xSemaphoreGive(g_touch_i2c_mtx);
        return false;
    }
    if (status & 0x80) g_gt_ready_count++;
    if ((status & 0x80) && ((status & 0x0F) > 0)) g_gt_points_count++;
    bool has_point = (status & 0x80) && ((status & 0x0F) > 0);

    if (has_point) {
        uint8_t point[6] = {0};
        if (gt911_read_regs_locked(0x8150, point, sizeof(point))) {
            (void)point[0];  // track id
            // GT911 on this panel returns coords high-byte-first within
            // each 16-bit field (empirically verified).
            uint8_t xh = point[1];
            uint8_t xl = point[2];
            uint8_t yh = point[3];
            uint8_t yl = point[4];
            *out_x = (int16_t)(((uint16_t)xh << 8) | xl);
            *out_y = (int16_t)(((uint16_t)yh << 8) | yl);
        } else {
            has_point = false;
        }
    }
    // Always clear status register so next sample fires
    gt911_write_reg_locked(0x814E, 0x00);
    if (g_touch_i2c_mtx) xSemaphoreGive(g_touch_i2c_mtx);
    return has_point;
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Waveshare knob: CST816S capacitive touch (I2C, addr 0x15) on the touch
// bus. The fbiego/CST816S driver is IRQ-driven: available() latches true on
// each controller event, and data.event is 0=Down / 1=Up / 2=Contact. We
// keep a sticky "pressed" latch so the existing 60 Hz touch_task poll path
// sees a synchronous pressed/coords read with the same contract as the
// GT911 path (it writes the shared g_touch snapshot under g_touch_mtx).
static CST816S g_cst816(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
static bool g_cst816_pressed = false;
static int16_t g_cst816_x = -1;
static int16_t g_cst816_y = -1;

static bool cst816_read_once(int16_t *out_x, int16_t *out_y) {
    bool locked = true;
    if (g_touch_i2c_mtx) {
        locked = xSemaphoreTake(g_touch_i2c_mtx, pdMS_TO_TICKS(20)) == pdTRUE;
    }
    if (!locked) return false;
    // Drain any pending controller event(s) and update the latch.
    while (g_cst816.available()) {
        // event: 0 = Down, 2 = Contact -> pressed; 1 = Up -> released.
        if (g_cst816.data.event == 1) {
            g_cst816_pressed = false;
        } else {
            g_cst816_pressed = true;
            g_cst816_x = (int16_t)g_cst816.data.x;
            g_cst816_y = (int16_t)g_cst816.data.y;
        }
    }
    if (g_touch_i2c_mtx) xSemaphoreGive(g_touch_i2c_mtx);
    if (g_cst816_pressed) {
        *out_x = g_cst816_x;
        *out_y = g_cst816_y;
    }
    return g_cst816_pressed;
}
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8

// Board-agnostic single-sample read: routes to the panel's touch
// controller (CST816S on the knob, GT911 on the Sunton/Guition panels).
static bool touch_read_point(int16_t *out_x, int16_t *out_y) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    return cst816_read_once(out_x, out_y);
#else
    return gt911_read_once(out_x, out_y);
#endif
}

static void gt911_dump_config() {
    static constexpr uint16_t CFG_START = 0x8047;
    static constexpr uint16_t CFG_END = 0x8100;
    static constexpr size_t CFG_LEN = CFG_END - CFG_START + 1;
    uint8_t cfg[CFG_LEN] = {0};

    if (!touch_present) {
        net::logf("[gt911] no controller present");
        return;
    }
    bool locked = true;
    if (g_touch_i2c_mtx) {
        locked = xSemaphoreTake(g_touch_i2c_mtx, pdMS_TO_TICKS(250)) == pdTRUE;
    }
    if (!locked) {
        net::logf("[gt911] config dump: I2C busy");
        return;
    }
    bool ok = gt911_read_regs_locked(CFG_START, cfg, sizeof(cfg));
    if (g_touch_i2c_mtx) xSemaphoreGive(g_touch_i2c_mtx);
    if (!ok) {
        net::logf("[gt911] config dump failed at addr=0x%02X", g_gt911_addr);
        return;
    }

    uint16_t x_max = (uint16_t)cfg[1] | ((uint16_t)cfg[2] << 8);
    uint16_t y_max = (uint16_t)cfg[3] | ((uint16_t)cfg[4] << 8);
    uint8_t module1 = cfg[6];
    uint8_t int_mode = module1 & 0x03;
    uint8_t touch_level = cfg[0x8053 - CFG_START];
    uint8_t leave_level = cfg[0x8054 - CFG_START];
    uint8_t refresh = cfg[0x8056 - CFG_START] & 0x0F;
    uint8_t x_threshold = cfg[0x8057 - CFG_START];
    uint8_t y_threshold = cfg[0x8058 - CFG_START];
    uint8_t border_tb = cfg[0x805B - CFG_START];
    uint8_t border_lr = cfg[0x805C - CFG_START];
    uint8_t chksum = cfg[0x80FF - CFG_START];
    uint8_t fresh = cfg[0x8100 - CFG_START];
    uint8_t sum = 0;
    for (size_t i = 0; i <= 0x80FE - CFG_START; ++i)
        sum += cfg[i];
    uint8_t calc = (uint8_t)((~sum) + 1);

    net::logf("[gt911] config addr=0x%02X range=0x%04X..0x%04X len=%u", g_gt911_addr, CFG_START,
              CFG_END, (unsigned)CFG_LEN);
    net::logf("[gt911] version=0x%02X x_max=%u y_max=%u touch_points=%u", cfg[0], (unsigned)x_max,
              (unsigned)y_max, (unsigned)(cfg[5] & 0x0F));
    net::logf("[gt911] module1=0x%02X int=%u axis_swap=%u sensor_rev=%u driver_rev=%u", module1,
              (unsigned)int_mode, (unsigned)((module1 >> 3) & 1), (unsigned)((module1 >> 4) & 1),
              (unsigned)((module1 >> 5) & 1));
    net::logf("[gt911] touch_level=%u leave_level=%u refresh=%u x_th=%u y_th=%u",
              (unsigned)touch_level, (unsigned)leave_level, (unsigned)refresh,
              (unsigned)x_threshold, (unsigned)y_threshold);
    net::logf("[gt911] border top=%u bottom=%u left=%u right=%u (units=32)",
              (unsigned)((border_tb >> 4) & 0x0F), (unsigned)(border_tb & 0x0F),
              (unsigned)((border_lr >> 4) & 0x0F), (unsigned)(border_lr & 0x0F));
    net::logf("[gt911] checksum stored=0x%02X calc=0x%02X fresh=0x%02X %s", chksum, calc, fresh,
              chksum == calc ? "OK" : "MISMATCH");

    for (size_t off = 0; off < CFG_LEN; off += 16) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[gt911] %04X:", (unsigned)(CFG_START + off));
        for (size_t i = 0; i < 16 && off + i < CFG_LEN; ++i) {
            n += snprintf(line + n, sizeof(line) - n, " %02X", cfg[off + i]);
        }
        net::logf("%s", line);
    }
}

// Forward decls for state owned later in the file but referenced by the
// touch-task swipe detector (which sits above those definitions).
static bool mob_is_active();
extern volatile uint32_t g_gesture_count;
extern volatile uint32_t g_gesture_suppressed;
extern char g_last_gesture[24];

// Swipe detection runs in touch_task (core 0) and is INDEPENDENT of
// LVGL. LVGL's own indev_gesture path can stall up to ~2s on the wind
// screen while the renderer composes transformed widgets, eating the
// swipe entirely. Detecting here and posting a ShowScreen command into
// app's UI queue means the swipe is captured even if LVGL is busy; the
// queued screen swap fires as soon as lv_timer_handler unblocks.
//
// Thresholds match docs/specs/06: 50 px min stroke, <500 ms, x or y
// component dominant. The LVGL gesture handler stays as a fallback for
// screens that aren't laggy; it suppresses itself within 500 ms of a
// touch_task swipe to avoid double-fire.
volatile uint32_t g_last_swipe_ms = 0;

// Thresholds from docs/specs/05-gesture-subsystem-spec.md - tightened
// from the earlier 50 px / 500 ms because finger drift on the GT911
// capacitive panel can reach 60-70 px during a "stationary" tap, which
// the loose thresholds were classifying as swipes and stealing taps
// from settings buttons.
//
//   SWIPE_MIN_PX:  80   (was 50 - within 80 px is a tap-or-drag, not a swipe)
//   SWIPE_MAX_MS:  1200 (was 500 - swipes on glove can be slow)
//   OFF_AXIS_MAX:  0.55 (new - reject diagonal motion as a directional swipe)
//   DEBOUNCE_MS:   250  (unchanged)
static constexpr int16_t SWIPE_MIN_PX = 80;
static constexpr uint32_t SWIPE_MAX_MS = 1200;
static constexpr uint32_t DEBOUNCE_MS = 250;

static void detect_swipe_release(int16_t down_x, int16_t down_y, uint32_t down_ms, int16_t up_x,
                                 int16_t up_y) {
    uint32_t now = millis();
    uint32_t dt = now - down_ms;
    if (dt == 0 || dt > SWIPE_MAX_MS) return;
    if (g_last_swipe_ms && (now - g_last_swipe_ms) < DEBOUNCE_MS) {
        net::logf("[touch] swipe debounced (%u ms since last)", (unsigned)(now - g_last_swipe_ms));
        return;
    }
    int32_t dx = (int32_t)up_x - down_x;
    int32_t dy = (int32_t)up_y - down_y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx < SWIPE_MIN_PX && ady < SWIPE_MIN_PX) return;

    // Off-axis filter: the dominant axis must be substantially larger
    // than the perpendicular axis (ratio < 0.55). Otherwise the motion
    // is ambiguous diagonal drift, not a deliberate swipe.
    const char *cmd = nullptr;
    const char *dir = nullptr;
    if (adx > ady) {
        if (ady * 100 > adx * 55) return;  // too diagonal for horizontal
        if (dx < 0) {
            cmd = "next";
            dir = "left";
        } else {
            cmd = "prev";
            dir = "right";
        }
    } else {
        if (adx * 100 > ady * 55) return;  // too diagonal for vertical
        if (dy < 0) {
            cmd = "settings";
            dir = "up";
        } else {
            cmd = "dashboard";
            dir = "down";
        }
    }

    // Per-screen policy (docs/specs/05-gesture-subsystem-spec.md "Screen-local maps"):
    const char *cur = ui::current_id();
    bool is_horizontal = (strcmp(cmd, "next") == 0 || strcmp(cmd, "prev") == 0);
    bool is_vertical = !is_horizontal;
    (void)is_vertical;

    // MOB overlay swallows all navigation - user must long-press CLEAR.
    if (mob_is_active()) {
        net::logf("[touch] swipe %s suppressed (mob active)", dir);
        g_gesture_suppressed++;
        return;
    }
    // Settings: only swipe-down (close) makes sense; L/R/up are no-ops.
    if (cur && strcmp(cur, "settings") == 0) {
        if (is_horizontal || strcmp(cmd, "settings") == 0) {
            net::logf("[touch] swipe %s suppressed on settings", dir);
            g_gesture_suppressed++;
            return;
        }
    }
    // WiFi keyboard: horizontal swipes collide with keyboard letter rows.
    // Vertical (up=settings, down=dashboard) remains as escape paths.
    if (cur && strcmp(cur, "wifi") == 0 && is_horizontal) {
        net::logf("[touch] swipe %s suppressed (wifi keyboard)", dir);
        g_gesture_suppressed++;
        return;
    }

    net::logf("[touch] swipe %s (dx=%d dy=%d dt=%u ms) -> %s", dir, (int)dx, (int)dy, (unsigned)dt,
              cmd);
    g_last_swipe_ms = now;
    g_gesture_count++;
    snprintf(g_last_gesture, sizeof(g_last_gesture), "swipe_%s", dir);
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, cmd, sizeof(c.a) - 1);
    // Stamp the post time at gesture detection so the CommandRtt
    // channel measures the actual touch-to-screen-switch latency,
    // not just the queue post->drain time.
    c.t_post_us = micros();
    app::post(c, 0);
}

static void touch_task(void *) {
    int16_t last_logged_x = -1, last_logged_y = -1;
    bool last_state = false;
    int16_t down_x = -1, down_y = -1;
    uint32_t down_ms = 0;
    for (;;) {
        if (touch_present) {
            if (g_touch_irq_enabled && !last_state) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            int16_t raw_x = -1, raw_y = -1;
            bool pressed = touch_read_point(&raw_x, &raw_y);
            int16_t x = raw_x, y = raw_y;
            if (pressed) {
                // Apply user calibration. Identity until user runs the
                // calibration screen, so this is a no-op out of the box.
                touch_cal::apply(&x, &y);
            }
            if (xSemaphoreTake(g_touch_mtx, pdMS_TO_TICKS(2))) {
                g_touch.pressed = pressed;
                if (pressed) {
                    g_touch.x = x;
                    g_touch.y = y;
                    g_touch.raw_x = raw_x;
                    g_touch.raw_y = raw_y;
                }
                g_touch.last_ms = millis();
                xSemaphoreGive(g_touch_mtx);
            }
            // Coarse-grained logging so user can see in BLE/UDP that taps
            // are landing on the IC. Throttled to changes.
            if (pressed &&
                (!last_state || abs(x - last_logged_x) > 20 || abs(y - last_logged_y) > 20)) {
                net::logf("[touch] DOWN raw=(%d,%d) cal=(%d,%d)", raw_x, raw_y, x, y);
                last_logged_x = x;
                last_logged_y = y;
            }
            // Track start of contact for the touch-task swipe detector.
            if (pressed && !last_state) {
                down_x = x;
                down_y = y;
                down_ms = millis();
            }
            if (!pressed && last_state) {
                net::logf("[touch] UP   raw=(%d,%d)", last_logged_x, last_logged_y);
                detect_swipe_release(down_x, down_y, down_ms, last_logged_x, last_logged_y);
            }
            last_state = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(16));  // ~60 Hz
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    if (!touch_present) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    TouchSnapshot snap = {0, 0, false, 0};
    if (g_touch_mtx && xSemaphoreTake(g_touch_mtx, pdMS_TO_TICKS(1))) {
        snap = g_touch;
        xSemaphoreGive(g_touch_mtx);
    }
    if (snap.pressed && snap.x >= 0 && snap.y >= 0) {
        data->point.x = snap.x;
        data->point.y = snap.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t lv_tick_cb(void) {
    return millis();
}

// ----- Test injection API (see include/input_test.h) ---------------------
// Implemented in this TU so it can reach the file-local g_touch /
// g_touch_mtx / detect_swipe_release. Always available - tests rely on
// it; if a deployment ever needs to lock this down, gate the web-side
// registrations in src/web.cpp, not these symbols.

#include "input_test.h"

namespace input_test {

bool is_injection_command(const String &line) {
    return line == "tap" || line.startsWith("tap ") || line == "swipe" ||
           line.startsWith("swipe ") || line == "gesture" || line.startsWith("gesture ") ||
           line == "touch" || line.startsWith("touch ");
}

#if YEYBOATS_ENABLE_INPUT_TEST
bool inject_touch(int16_t x, int16_t y, bool pressed) {
    if (!g_touch_mtx) return false;
    if (xSemaphoreTake(g_touch_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    g_touch.pressed = pressed;
    if (pressed) {
        g_touch.x = x;
        g_touch.y = y;
        g_touch.raw_x = x;
        g_touch.raw_y = y;
    }
    g_touch.last_ms = millis();
    xSemaphoreGive(g_touch_mtx);
    net::logf("[test] inject_touch x=%d y=%d pressed=%d", x, y, pressed);
    return true;
}

bool inject_tap(int16_t x, int16_t y, uint32_t hold_ms) {
    if (hold_ms < 20) hold_ms = 20;      // LVGL indev polls ~5 ms; give it time
    if (hold_ms > 2000) hold_ms = 2000;  // sanity
    if (!inject_touch(x, y, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return inject_touch(0, 0, false);
}

bool inject_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t dur_ms, uint8_t steps) {
    if (steps < 2) steps = 2;
    if (steps > 32) steps = 32;
    if (dur_ms < 20) dur_ms = 20;
    if (dur_ms > 3000) dur_ms = 3000;
    uint32_t per_step = dur_ms / steps;
    uint32_t down_ms = millis();
    // Intermediate samples - LVGL sees motion and updates its drag state.
    for (uint8_t i = 0; i <= steps; ++i) {
        int32_t x = x0 + ((int32_t)(x1 - x0) * i) / steps;
        int32_t y = y0 + ((int32_t)(y1 - y0) * i) / steps;
        inject_touch((int16_t)x, (int16_t)y, true);
        vTaskDelay(pdMS_TO_TICKS(per_step));
    }
    inject_touch(0, 0, false);
    // Drive the project's high-level swipe detector directly. The
    // touch_task also calls this on real GT911 release transitions; we
    // call it here so injection doesn't depend on whether the touch
    // task happened to be polling at our release moment.
    detect_swipe_release(x0, y0, down_ms, x1, y1);
    return true;
}

bool post_gesture(const char *dir) {
    if (!dir) return false;
    const char *cmd = nullptr;
    if (!strcmp(dir, "left"))
        cmd = "next";
    else if (!strcmp(dir, "right"))
        cmd = "prev";
    else if (!strcmp(dir, "up"))
        cmd = "settings";
    else if (!strcmp(dir, "down"))
        cmd = "dashboard";
    else
        return false;
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, cmd, sizeof(c.a) - 1);
    c.t_post_us = micros();
    net::logf("[test] post_gesture dir=%s -> %s", dir, cmd);
    return app::post(c, 0);
}

namespace {

// Pull N whitespace-separated integers off the front of `s`. Returns
// the number successfully parsed (up to `max_n`). Tokens past the
// requested count are ignored.
int parse_ints(const String &s, int *out, int max_n) {
    int n = 0;
    int i = 0;
    int len = s.length();
    while (i < len && n < max_n) {
        while (i < len && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= len) break;
        int start = i;
        if (s[i] == '-' || s[i] == '+') ++i;
        bool any = false;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            ++i;
            any = true;
        }
        if (!any) break;
        out[n++] = s.substring(start, i).toInt();
    }
    return n;
}

}  // namespace

bool handleConsoleCommand(const String &line) {
    if (!is_injection_command(line)) return false;

    int sp = line.indexOf(' ');
    String head = sp < 0 ? line : line.substring(0, sp);
    String rest = sp < 0 ? String("") : line.substring(sp + 1);
    rest.trim();

    int args[6] = {0};

    if (head == "touch") {
        int n = parse_ints(rest, args, 3);
        if (n < 3) {
            net::logf("[test] usage: touch <x> <y> <0|1>");
            return true;
        }
        bool ok = inject_touch((int16_t)args[0], (int16_t)args[1], args[2] != 0);
        net::logf("[test] touch ok=%d", ok);
        return true;
    }
    if (head == "tap") {
        int n = parse_ints(rest, args, 3);
        if (n < 2) {
            net::logf("[test] usage: tap <x> <y> [hold_ms]");
            return true;
        }
        uint32_t hold = n >= 3 ? (uint32_t)args[2] : 50;
        bool ok = inject_tap((int16_t)args[0], (int16_t)args[1], hold);
        net::logf("[test] tap ok=%d x=%d y=%d hold=%lu", ok, args[0], args[1], (unsigned long)hold);
        return true;
    }
    if (head == "swipe") {
        int n = parse_ints(rest, args, 6);
        if (n < 4) {
            net::logf("[test] usage: swipe <x0> <y0> <x1> <y1> [dur_ms] [steps]");
            return true;
        }
        uint32_t dur = n >= 5 ? (uint32_t)args[4] : 300;
        uint8_t steps = n >= 6 ? (uint8_t)args[5] : 8;
        bool ok = inject_swipe((int16_t)args[0], (int16_t)args[1], (int16_t)args[2],
                               (int16_t)args[3], dur, steps);
        net::logf("[test] swipe ok=%d (%d,%d)->(%d,%d) dur=%lu steps=%u", ok, args[0], args[1],
                  args[2], args[3], (unsigned long)dur, (unsigned)steps);
        return true;
    }
    if (head == "gesture") {
        bool ok = post_gesture(rest.c_str());
        net::logf("[test] gesture ok=%d dir=%s", ok, rest.c_str());
        return true;
    }
    return false;  // unreachable
}
#else
bool inject_touch(int16_t, int16_t, bool) {
    return false;
}
bool inject_tap(int16_t, int16_t, uint32_t) {
    return false;
}
bool inject_swipe(int16_t, int16_t, int16_t, int16_t, uint32_t, uint8_t) {
    return false;
}
bool post_gesture(const char *) {
    return false;
}
bool handleConsoleCommand(const String &line) {
    return is_injection_command(line);
}
#endif

}  // namespace input_test

// ----- Global overlays (visible on every screen) -------------------------
// MOB button + rescue overlay
static struct {
    bool active = false;
    double lat = NAN, lon = NAN;
    uint32_t trigger_ms = 0;
} g_mob;
static bool mob_is_active() {
    return g_mob.active;
}
static lv_obj_t *mob_view = nullptr;
static lv_obj_t *mob_lbl_dist, *mob_lbl_brg, *mob_lbl_back, *mob_lbl_elapsed;

// Alarm banner
// Spec 17 §8: ALARM_MGR_OVERLAY carries a transient message pushed
// from the manager via overlay.show. While it is active, alarm_check
// is suppressed so a data-driven alarm doesn't overwrite the operator
// message; overlay_clear releases the suppression.
enum AlarmId {
    ALARM_NONE = 0,
    ALARM_DEPTH_SHALLOW,
    ALARM_SK_STALLED,
    ALARM_SK_NODATA,
    ALARM_BATT_LOW,
    ALARM_MGR_OVERLAY
};
static bool g_overlay_pinned = false;
static lv_obj_t *alarm_banner = nullptr;
static lv_obj_t *alarm_label = nullptr;
// Active alarms share ONE blinking banner: when several fire they rotate through
// the same slot (cycled + blinked by g_alarm_timer on the LVGL task). Filled by
// alarm_check() or, while a manager overlay is pinned, by overlay_show().
struct ActiveAlarm {
    AlarmId id;
    const char *msg;
};
static ActiveAlarm g_active_alarms[6];
static int g_active_alarm_n = 0;
static int g_alarm_cycle = 0;
static lv_timer_t *g_alarm_timer = nullptr;
// Defined below alarms_build (which creates the timer pointing at it).
static void alarm_tick(lv_timer_t *);

// FPS overlay
static lv_obj_t *g_fps_overlay = nullptr;
static float g_fps = 0.0f;
static uint32_t g_fps_peak_us = 0;
static float g_fps_avg_us = 0.0f;
#ifdef DBG_PERF_COUNTERS
static volatile uint32_t g_refresh_us = 0;  // last ui::refresh_current() duration
#endif

// Demo mode
static lv_timer_t *g_demo_timer = nullptr;
static uint32_t g_demo_period_ms = 3000;
static lv_obj_t *g_demo_badge = nullptr;

static double mob_dist_m() {
    if (!g_mob.active || isnan(sk::data.lat) || isnan(sk::data.lon)) return NAN;
    const double R = 6371000.0;
    double dlat = (g_mob.lat - sk::data.lat) * M_PI / 180.0;
    double dlon = (g_mob.lon - sk::data.lon) * M_PI / 180.0;
    double l1 = sk::data.lat * M_PI / 180.0;
    double l2 = g_mob.lat * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(l1) * cos(l2) * sin(dlon / 2) * sin(dlon / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static double mob_brg_deg() {
    if (!g_mob.active || isnan(sk::data.lat) || isnan(sk::data.lon)) return NAN;
    double phi1 = sk::data.lat * M_PI / 180.0;
    double phi2 = g_mob.lat * M_PI / 180.0;
    double dlon = (g_mob.lon - sk::data.lon) * M_PI / 180.0;
    double y = sin(dlon) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlon);
    double b = atan2(y, x) * 180.0 / M_PI;
    while (b < 0)
        b += 360;
    return b;
}

static void mob_trigger() {
    if (isnan(sk::data.lat) || isnan(sk::data.lon)) {
        net::logf("[mob] no GPS fix - cannot mark");
        return;
    }
    g_mob.active = true;
    g_mob.lat = sk::data.lat;
    g_mob.lon = sk::data.lon;
    g_mob.trigger_ms = millis();
    net::logf("[mob] MARK at %+.5f %+.5f", g_mob.lat, g_mob.lon);
    if (mob_view) lv_obj_clear_flag(mob_view, LV_OBJ_FLAG_HIDDEN);
}

static void mob_clear() {
    if (!g_mob.active) return;
    g_mob.active = false;
    net::logf("[mob] cleared");
    if (mob_view) lv_obj_add_flag(mob_view, LV_OBJ_FLAG_HIDDEN);
}

static void mob_clear_evt(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) mob_clear();
}

static void mob_refresh() {
    if (!g_mob.active) return;
    char buf[64];
    double d = mob_dist_m();
    if (!isnan(d)) {
        if (d >= 1000)
            snprintf(buf, sizeof(buf), "%.2f km", d / 1000.0);
        else
            snprintf(buf, sizeof(buf), "%.0f m", d);
        lv_label_set_text(mob_lbl_dist, buf);
    }
    double b = mob_brg_deg();
    if (!isnan(b)) {
        snprintf(buf, sizeof(buf), "BRG %03.0f\xC2\xB0", b);
        lv_label_set_text(mob_lbl_brg, buf);
        double back = b + 180.0;
        while (back >= 360)
            back -= 360;
        snprintf(buf, sizeof(buf), "return %03.0f\xC2\xB0", back);
        lv_label_set_text(mob_lbl_back, buf);
    }
    uint32_t s = (millis() - g_mob.trigger_ms) / 1000;
    snprintf(buf, sizeof(buf), "elapsed %02lu:%02lu", (unsigned long)(s / 60),
             (unsigned long)(s % 60));
    lv_label_set_text(mob_lbl_elapsed, buf);
}

static void mob_build(lv_obj_t *scr) {
    // The on-screen MOB button was removed per product direction. MOB is still
    // triggered via the console/command path (mob_trigger), which shows the
    // rescue overlay built below; the overlay's own HOLD-TO-CLEAR remains.
    mob_view = lv_obj_create(scr);
    lv_obj_set_size(mob_view, LCD_W, LCD_H);
    lv_obj_set_pos(mob_view, 0, 0);
    lv_obj_set_style_bg_color(mob_view, lv_color_hex(0x1a0000), 0);
    lv_obj_set_style_border_width(mob_view, 4, 0);
    lv_obj_set_style_border_color(mob_view, lv_color_hex(ui::theme.alarm), 0);
    lv_obj_set_style_radius(mob_view, 0, 0);
    lv_obj_set_style_pad_all(mob_view, 16, 0);
    lv_obj_clear_flag(mob_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mob_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = lv_label_create(mob_view);
    lv_label_set_text(t, "MAN OVERBOARD");
    lv_obj_set_style_text_color(t, lv_color_hex(ui::theme.port), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    mob_lbl_dist = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_dist, "--- m");
    lv_obj_set_style_text_color(mob_lbl_dist, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mob_lbl_dist, &lv_font_montserrat_48, 0);
    lv_obj_align(mob_lbl_dist, LV_ALIGN_TOP_MID, 0, 70);

    mob_lbl_brg = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_brg, "BRG ---\xC2\xB0");
    lv_obj_set_style_text_color(mob_lbl_brg, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mob_lbl_brg, &lv_font_montserrat_48, 0);
    lv_obj_align(mob_lbl_brg, LV_ALIGN_CENTER, 0, 10);

    mob_lbl_back = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_back, "return ---\xC2\xB0");
    lv_obj_set_style_text_color(mob_lbl_back, lv_color_hex(ui::theme.accent), 0);
    lv_obj_set_style_text_font(mob_lbl_back, &lv_font_montserrat_20, 0);
    lv_obj_align(mob_lbl_back, LV_ALIGN_CENTER, 0, 70);

    mob_lbl_elapsed = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_elapsed, "elapsed --:--");
    lv_obj_set_style_text_color(mob_lbl_elapsed, lv_color_hex(ui::theme.fg), 0);
    lv_obj_set_style_text_font(mob_lbl_elapsed, &lv_font_montserrat_20, 0);
    lv_obj_align(mob_lbl_elapsed, LV_ALIGN_BOTTOM_MID, 0, -70);

    lv_obj_t *clear_btn = lv_obj_create(mob_view);
    lv_obj_set_size(clear_btn, 220, 56);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(clear_btn, 8, 0);
    lv_obj_set_style_pad_all(clear_btn, 0, 0);
    lv_obj_clear_flag(clear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clear_btn, mob_clear_evt, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *cbl = lv_label_create(clear_btn);
    lv_label_set_text(cbl, "HOLD TO CLEAR");
    lv_obj_set_style_text_color(cbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cbl, &lv_font_montserrat_14, 0);
    lv_obj_center(cbl);
}

static void alarms_build(lv_obj_t *scr) {
    alarm_banner = lv_obj_create(scr);
    lv_obj_set_size(alarm_banner, 300, 34);
    // TOP-center, not BOTTOM: the bottom-center is occupied by data tiles on the
    // wind/compass HUDs, which the banner used to cover. The top strip is clear.
    lv_obj_align(alarm_banner, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(alarm_banner, lv_color_hex(ui::theme.alarm), 0);
    lv_obj_set_style_border_color(alarm_banner, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(alarm_banner, 1, 0);
    lv_obj_set_style_radius(alarm_banner, 6, 0);
    lv_obj_set_style_pad_all(alarm_banner, 0, 0);
    lv_obj_clear_flag(alarm_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alarm_banner, LV_OBJ_FLAG_HIDDEN);
    alarm_label = lv_label_create(alarm_banner);
    lv_label_set_text(alarm_label, "");
    lv_obj_set_style_text_color(alarm_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(alarm_label, &lv_font_montserrat_20, 0);
    lv_obj_center(alarm_label);
    // Blink + multi-alarm cycle driver (no-op while nothing is active).
    if (!g_alarm_timer) g_alarm_timer = lv_timer_create(alarm_tick, 500, nullptr);
}

// Show the current cycle entry (or hide when nothing is active).
static void alarm_render() {
    if (!alarm_banner) return;
    if (g_active_alarm_n == 0) {
        lv_obj_add_flag(alarm_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (g_alarm_cycle >= g_active_alarm_n) g_alarm_cycle = 0;
    lv_label_set_text(alarm_label, g_active_alarms[g_alarm_cycle].msg);
    lv_obj_clear_flag(alarm_banner, LV_OBJ_FLAG_HIDDEN);
}

// Blink (opacity pulse) + rotate through multiple active alarms. lv_timer on the
// LVGL task, so lv_obj_* mutations are safe. 500 ms blink; advance every 3 ticks
// (~1.5 s) when more than one alarm is active.
static void alarm_tick(lv_timer_t *) {
    static uint8_t phase = 0, cyc = 0;
    if (g_active_alarm_n == 0 || !alarm_banner) return;
    phase ^= 1;
    lv_obj_set_style_bg_opa(alarm_banner, phase ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_text_opa(alarm_label, phase ? LV_OPA_COVER : LV_OPA_50, 0);
    if (g_active_alarm_n > 1 && ++cyc >= 3) {
        cyc = 0;
        g_alarm_cycle = (g_alarm_cycle + 1) % g_active_alarm_n;
        lv_label_set_text(alarm_label, g_active_alarms[g_alarm_cycle].msg);
    }
}

// Replace the active-alarm set; reset the cycle + log only when it actually
// changes (alarm_check runs at 5 Hz). Used by alarm_check and overlay_show.
static void alarm_set_list(const ActiveAlarm *list, int n) {
    if (n > 6) n = 6;
    bool changed = (n != g_active_alarm_n);
    for (int i = 0; !changed && i < n; ++i)
        if (g_active_alarms[i].id != list[i].id) changed = true;
    for (int i = 0; i < n; ++i)
        g_active_alarms[i] = list[i];
    g_active_alarm_n = n;
    if (changed) {
        g_alarm_cycle = 0;
        if (n == 0)
            net::logf("[alarm] cleared");
        else
            for (int i = 0; i < n; ++i)
                net::logf("[alarm] %s", list[i].msg);
    }
    alarm_render();
}

static void alarm_check() {
    // While a manager overlay is pinned, leave the banner alone so the
    // operator message isn't overwritten by data-driven alarms.
    if (g_overlay_pinned) return;
    ActiveAlarm a[6];
    int n = 0;
    if (!isnan(sk::data.depth) && sk::data.depth > 0 && sk::data.depth < ui::depth_alarm_m())
        a[n++] = {ALARM_DEPTH_SHALLOW, "SHALLOW WATER"};
    String sk_state = sk::connectionStatus();
    if (sk_state == "stalled")
        a[n++] = {ALARM_SK_STALLED, "SIGNALK STALLED"};
    else if (sk_state == "no-data")
        a[n++] = {ALARM_SK_NODATA, "SIGNALK NO DATA"};
    if (!isnan(sk::data.battVoltage) && sk::data.battVoltage < ui::battery_alarm_v())
        a[n++] = {ALARM_BATT_LOW, "BATTERY LOW"};
    alarm_set_list(a, n);
}

// Spec 17 §8 overlay primitives. Must run on the LVGL task (via the
// app::Command pump path); calling from any worker task corrupts LVGL
// state - see CLAUDE.md memory traps. Posting the matching app::Command
// is the only safe entry point.
bool main_set_touch_mode(const char *mode) {
    if (!mode || !*mode) return false;
    bool want_irq = false;
    if (!strcmp(mode, "poll"))
        want_irq = false;
    else if (!strcmp(mode, "irq"))
        want_irq = true;
    else
        return false;

    if (want_irq == g_touch_irq_enabled) return true;  // already there

    if (want_irq) {
        // Can only enable IRQ if hardware + worker task are both ready.
        if (TOUCH_INT < 0 || !g_touch_task) return false;
#if TOUCH_INT_ACTIVE_LOW
        pinMode(TOUCH_INT, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_irq_isr, FALLING);
#else
        pinMode(TOUCH_INT, INPUT_PULLDOWN);
        attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_irq_isr, RISING);
#endif
        g_touch_irq_enabled = true;
        net::logf("[touch] mode -> irq (gpio %d)", TOUCH_INT);
    } else {
        if (TOUCH_INT >= 0) detachInterrupt(digitalPinToInterrupt(TOUCH_INT));
        g_touch_irq_enabled = false;
        net::logf("[touch] mode -> poll");
    }
    return true;
}

namespace ui {

void overlay_show(const char *message) {
    g_overlay_pinned = true;
    // Pinned manager overlay = a single banner entry (no data-alarm cycling).
    ActiveAlarm a = {ALARM_MGR_OVERLAY, message && *message ? message : "OVERLAY"};
    alarm_set_list(&a, 1);
}

void overlay_clear() {
    g_overlay_pinned = false;
    alarm_set_list(nullptr, 0);
    // Let the next ui_refresh tick re-evaluate auto-alarms.
}

}  // namespace ui

// ----- Demo / fps -------------------------------------------------------

static void demo_tick(lv_timer_t *) {
    ui::next();
}

static void demo_start(uint32_t period_ms) {
    g_demo_period_ms = period_ms ? period_ms : 3000;
    if (!g_demo_badge) {
        g_demo_badge = lv_label_create(lv_layer_top());
        lv_label_set_text(g_demo_badge, " DEMO ");
        lv_obj_set_style_bg_color(g_demo_badge, lv_color_hex(ui::theme.port), 0);
        lv_obj_set_style_bg_opa(g_demo_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(g_demo_badge, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(g_demo_badge, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(g_demo_badge, 4, 0);
        lv_obj_set_style_radius(g_demo_badge, 4, 0);
    }
    lv_obj_clear_flag(g_demo_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(g_demo_badge, LV_ALIGN_TOP_LEFT, 6, 6);
    if (g_demo_timer) lv_timer_del(g_demo_timer);
    g_demo_timer = lv_timer_create(demo_tick, g_demo_period_ms, NULL);
    net::logf("[demo] started, %lu ms per step", (unsigned long)g_demo_period_ms);
}

static void demo_stop() {
    if (g_demo_timer) {
        lv_timer_del(g_demo_timer);
        g_demo_timer = nullptr;
    }
    if (g_demo_badge) lv_obj_add_flag(g_demo_badge, LV_OBJ_FLAG_HIDDEN);
    ui::show(0);
    net::logf("[demo] stopped");
}

#ifdef DBG_PERF_COUNTERS
static void sweep_tick();  // bench-sweep state machine (impl below the peak globals)
#endif

static void fps_tick(lv_timer_t *) {
    g_fps = (float)g_flush_count;
    g_fps_peak_us = g_flush_us_peak;
    g_fps_avg_us = g_flush_count ? (float)g_flush_us_total / g_flush_count : 0.0f;
    g_flush_count = 0;
    g_flush_us_total = 0;
    g_flush_us_peak = 0;
    if (g_fps_overlay && !lv_obj_has_flag(g_fps_overlay, LV_OBJ_FLAG_HIDDEN)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f Hz  avg %.0fus  peak %luus", g_fps, g_fps_avg_us,
                 (unsigned long)g_fps_peak_us);
        lv_label_set_text(g_fps_overlay, buf);
    }
#ifdef DBG_PERF_COUNTERS
    sweep_tick();
#endif
}

static void fps_overlay_toggle() {
    if (!g_fps_overlay) {
        g_fps_overlay = lv_label_create(lv_layer_top());
        lv_label_set_text(g_fps_overlay, "-- Hz");
        lv_obj_set_style_bg_color(g_fps_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g_fps_overlay, LV_OPA_70, 0);
        lv_obj_set_style_text_color(g_fps_overlay, lv_color_hex(0x33ff99), 0);
        lv_obj_set_style_text_font(g_fps_overlay, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(g_fps_overlay, 4, 0);
        lv_obj_set_style_radius(g_fps_overlay, 4, 0);
        lv_obj_align(g_fps_overlay, LV_ALIGN_TOP_LEFT, 70, 4);
        return;
    }
    if (lv_obj_has_flag(g_fps_overlay, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(g_fps_overlay, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_fps_overlay, LV_OBJ_FLAG_HIDDEN);
}

// Metrics maintained inside loop()
static volatile uint32_t g_loop_last_ms = 0;
static volatile uint32_t g_loop_max_us = 0;
static volatile uint32_t g_lvgl_max_us = 0;
static volatile uint32_t g_section_max_us = 0;
static char g_section_peak_name[24] = "-";
// Default changed from true -> false per docs/specs/09 "remove or
// scope forced full-screen invalidation". With per-screen dirty-value
// caches in place (see screen_wind.cpp), continuously invalidating
// the active screen at 5 Hz just defeats LVGL's partial-render mode.
// Re-enable per-screen via `force-invalidate on` if a screen needs
// continuous time-based redraws.
static volatile bool g_force_invalidate = false;

static void note_slow_section(const char *name, uint32_t dt_us) {
    if (dt_us <= g_section_max_us) return;
    g_section_max_us = dt_us;
    strncpy(g_section_peak_name, name ? name : "?", sizeof(g_section_peak_name) - 1);
    g_section_peak_name[sizeof(g_section_peak_name) - 1] = 0;
}

#ifdef DBG_PERF_COUNTERS
// ---- bench-sweep: walk every non-hidden screen x {typed, store} render mode,
// dwell SWEEP_DWELL s (first tick discarded), and log a per-cell metrics row.
// Driven by fps_tick (1 Hz), so it runs on the LVGL/UI task (ui::show is safe).
namespace ui::layouts {
extern bool g_bench_store_mode;  // shadow-resolve built-ins through PathStore
}

static const int SWEEP_DWELL = 4;  // seconds per cell (tick 0 = settle, discarded)
static bool g_sweep_active = false;
static int g_sweep_mode = 0;  // 0 = typed, 1 = store
static int g_sweep_pos = 0;   // index into g_sweep_list
static int g_sweep_tick = 0;
static int g_sweep_list[32];
static int g_sweep_n = 0;
static int g_sweep_saved = 0;
static bench::BenchRow g_sweep_row;

static void sweep_build_list() {
    g_sweep_n = 0;
    size_t n = ui::screen_count();
    for (size_t i = 0; i < n && g_sweep_n < 32; ++i) {
        const char *id = nullptr, *t = nullptr;
        bool hid = false;
        if (ui::screen_info((int)i, &id, &t, &hid) && !hid) g_sweep_list[g_sweep_n++] = (int)i;
    }
}

static bench::BenchSample sweep_sample() {
    bench::BenchSample s{};
    s.fps = g_fps;
    s.flush_avg_us = g_fps_avg_us;
    s.flush_peak_us = (double)g_fps_peak_us;
    s.refresh_us = (double)g_refresh_us;
    s.first_frame_us = (double)g_ff_us;  // show -> first flush (load latency), per cell
    s.build_us = (double)ui::build_us(g_sweep_list[g_sweep_pos]);  // cold build cost
    s.lvgl_peak_us = (double)g_lvgl_max_us;
    s.loop_peak_us = (double)g_loop_max_us;
    s.core0_idle_pct = -1;  // n/a without FreeRTOS runtime stats (see plan)
    s.core1_idle_pct = -1;
    sk::BenchNet bn = sk::benchNetTake();
    s.deltas_s = bn.deltas;
    s.parsed_s = bn.parsed;
    s.parse_avg_us = bn.ws_frames ? (double)bn.parse_us_total / bn.ws_frames : 0.0;
    s.store_size = bn.store_size;
    s.store_lookups_s = bn.store_lookups;
    s.ws_frames_s = bn.ws_frames;
    s.ws_bytes_s = bn.ws_bytes;
    s.subscriptions = bn.subscriptions;
    s.heap_lowwater_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0;
    s.largest_block_kb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0;
    s.min_stack_b = (double)uxTaskGetStackHighWaterMark(NULL);
    s.temp_c = temperatureRead();
    // reset the loop/lvgl peaks so each cell reports its own peak, not a carryover
    g_loop_max_us = 0;
    g_lvgl_max_us = 0;
    return s;
}

static void sweep_show_current() {
    if (g_sweep_pos < g_sweep_n) {
        g_ff_us = 0;
        g_ff_show_us = micros();
        g_ff_arm = true;  // disp_flush_cb records the first frame after this show
        ui::show(g_sweep_list[g_sweep_pos]);
    }
}

static void bench_sweep_start() {
    sweep_build_list();
    if (g_sweep_n == 0) {
        net::logf("[bench-sweep] no non-hidden screens");
        return;
    }
    g_sweep_saved = ui::current_index();
    g_sweep_mode = 0;
    ui::layouts::g_bench_store_mode = false;
    g_sweep_pos = 0;
    g_sweep_tick = 0;
    g_sweep_row.reset();
    char hdr[300];
    bench::BenchRow::header(hdr, sizeof(hdr));
    net::logf("[bench-sweep] start: %d screens x 2 modes, %d s dwell (tick 1 discarded)", g_sweep_n,
              SWEEP_DWELL);
    net::logf("[bench-sweep] %s", hdr);
    sweep_show_current();
    g_sweep_active = true;
}

static void sweep_finish_cell() {
    const char *sid = nullptr, *t = nullptr;
    bool hid = false;
    ui::screen_info(g_sweep_list[g_sweep_pos], &sid, &t, &hid);
    char label[64];
    snprintf(label, sizeof(label), "%s,%s", sid ? sid : "?", g_sweep_mode ? "store" : "typed");
    char row[320];
    g_sweep_row.format(label, row, sizeof(row));
    net::logf("[bench-sweep] %s", row);

    g_sweep_tick = 0;
    g_sweep_row.reset();
    ++g_sweep_pos;
    if (g_sweep_pos >= g_sweep_n) {
        g_sweep_pos = 0;
        if (g_sweep_mode == 0) {
            g_sweep_mode = 1;
            ui::layouts::g_bench_store_mode = true;  // A/B: route built-ins via store
            sweep_show_current();
        } else {
            net::logf("[bench-sweep] complete");
            g_sweep_active = false;
            ui::layouts::g_bench_store_mode = false;
            ui::show(g_sweep_saved);
        }
        return;
    }
    sweep_show_current();
}

static void sweep_tick() {
    if (!g_sweep_active) return;
    if (g_sweep_tick == 0) {
        sk::benchNetTake();  // flush the screen-switch transient interval
    } else {
        g_sweep_row.add(sweep_sample());
    }
    ++g_sweep_tick;
    if (g_sweep_tick > SWEEP_DWELL) sweep_finish_cell();
}
#endif  // DBG_PERF_COUNTERS

// Forward decls for gesture diagnostics defined below.
extern volatile uint32_t g_gesture_count;
extern volatile uint32_t g_gesture_suppressed;
extern char g_last_gesture[24];

static void bench_dump() {
    size_t heap_free = esp_get_free_heap_size();
    size_t heap_lowwater = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    net::logf("[bench] fps=%.1f Hz", g_fps);
    net::logf("[bench] flush avg=%.0f us  peak=%lu us", g_fps_avg_us, (unsigned long)g_fps_peak_us);
    net::logf("[bench] screen=%s cold-build=%lu us", ui::current_id(),
              (unsigned long)ui::build_us(ui::current_index()));
    net::logf("[bench] loop peak=%lu us  slow section=%s %lu us  lvgl peak=%lu us",
              (unsigned long)g_loop_max_us, g_section_peak_name, (unsigned long)g_section_max_us,
              (unsigned long)g_lvgl_max_us);
    net::logf("[bench] heap free=%u KB  low-water=%u KB", (unsigned)(heap_free / 1024),
              (unsigned)(heap_lowwater / 1024));
    net::logf("[bench] psram free=%u / %u KB", (unsigned)(psram_free / 1024),
              (unsigned)(psram_total / 1024));
    {
        float tC = board::chipTempC();
        if (!isnan(tC))
            net::logf("[bench] chip temp=%.1f C", tC);
        else
            net::logf("[bench] chip temp=n/a");
    }
    net::logf("[bench] queues ui=%u (hi=%lu)  net=%u (hi=%lu)", (unsigned)app::ui_queue_depth(),
              (unsigned long)app::ui_high_water(), (unsigned)app::net_queue_depth(),
              (unsigned long)app::net_high_water());
    net::logf("[bench] wifi: %s  sk: %s", net::wifiStateName(), sk::connectionStatus().c_str());
    net::logf("[bench] gestures: %lu (sup %lu)  last=%s", (unsigned long)g_gesture_count,
              (unsigned long)g_gesture_suppressed, g_last_gesture[0] ? g_last_gesture : "-");
    net::logf("[bench] touch: mode=%s irq=%lu i2c ok=%lu err=%lu ready=%lu points=%lu",
              main_touch_mode(), (unsigned long)g_touch_irq_count, (unsigned long)g_i2c_ok_count,
              (unsigned long)g_i2c_err_count, (unsigned long)g_gt_ready_count,
              (unsigned long)g_gt_points_count);
    // Render volume (docs/specs/09): flushed pixels since last reset
    // and largest single flush rect. 480*480 = 230400 px is a "full
    // screen" reference.
    uint64_t flushed = g_flush_px_total;
    uint32_t flush_peak = g_flush_px_peak;
    net::logf(
        "[bench] render: flushed=%llu px (~%llu full screens) peak_rect=%lu px (%.1f%% screen)",
        (unsigned long long)flushed, (unsigned long long)(flushed / 230400ULL),
        (unsigned long)flush_peak, flush_peak * 100.0f / 230400.0f);
    // Latency channels (docs/specs/09). Each line: count, min, avg, max
    // in microseconds. Persist across bench dumps so a long sample is
    // possible; use `latency-reset` to zero the histograms.
    for (int i = 0; i < (int)latency::Channel::COUNT; ++i) {
        latency::Channel ch = (latency::Channel)i;
        latency::Stats s = latency::snapshot(ch);
        if (s.count == 0) {
            net::logf("[bench] lat %-14s n=0", latency::channel_name(ch));
        } else {
            uint32_t avg = (uint32_t)(s.sum_us / s.count);
            net::logf("[bench] lat %-14s n=%lu  min=%lu us  avg=%lu us  max=%lu us",
                      latency::channel_name(ch), (unsigned long)s.count, (unsigned long)s.min_us,
                      (unsigned long)avg, (unsigned long)s.max_us);
        }
    }
    // Reset peak counters so next bench shows recent activity.
    g_loop_max_us = 0;
    g_lvgl_max_us = 0;
    g_section_max_us = 0;
    g_flush_px_total = 0;
    g_flush_px_peak = 0;
    strncpy(g_section_peak_name, "-", sizeof(g_section_peak_name) - 1);
    g_section_peak_name[sizeof(g_section_peak_name) - 1] = 0;
}

// ----- Gesture handler --------------------------------------------------
// Diagnostics for /api/state.gestures.* and `bench`. Declared before
// bench_dump etc. above; defined here.
extern volatile uint32_t g_gesture_count;
extern volatile uint32_t g_gesture_suppressed;
extern char g_last_gesture[24];
volatile uint32_t g_gesture_count = 0;
volatile uint32_t g_gesture_suppressed = 0;
char g_last_gesture[24] = "";
extern "C" {
uint32_t main_gesture_count() {
    return g_gesture_count;
}
uint32_t main_gesture_suppressed() {
    return g_gesture_suppressed;
}
const char *main_last_gesture() {
    return g_last_gesture;
}
void main_touch_state(int *x, int *y, int *pressed, uint32_t *last_ms) {
    TouchSnapshot snap = {-1, -1, false, 0, -1, -1};
    if (g_touch_mtx && xSemaphoreTake(g_touch_mtx, pdMS_TO_TICKS(2))) {
        snap = g_touch;
        xSemaphoreGive(g_touch_mtx);
    }
    if (x) *x = snap.x;
    if (y) *y = snap.y;
    if (pressed) *pressed = snap.pressed ? 1 : 0;
    if (last_ms) *last_ms = snap.last_ms;
}
// Raw (pre-calibration) coords for the calibration screen.
void main_touch_raw(int *raw_x, int *raw_y, int *pressed) {
    TouchSnapshot snap = {-1, -1, false, 0, -1, -1};
    if (g_touch_mtx && xSemaphoreTake(g_touch_mtx, pdMS_TO_TICKS(2))) {
        snap = g_touch;
        xSemaphoreGive(g_touch_mtx);
    }
    if (raw_x) *raw_x = snap.raw_x;
    if (raw_y) *raw_y = snap.raw_y;
    if (pressed) *pressed = snap.pressed ? 1 : 0;
}
}

extern volatile uint32_t g_last_swipe_ms;

// Slice 3 screen-change hook: build the active screen's desired path-set and
// publish it to the SK subscription manager. `collect` is the screen's
// CollectPathsFn (NULL for fullscreen HUDs that read sk::data directly, which
// fall back to the full baseline so they keep all their data). The scratch set
// is function-static: this runs serially on the UI/LVGL task, and a fresh
// ~5 KB SubscriptionSet on the loopTask stack would overflow it (CLAUDE.md
// large-struct-on-stack trap).
static void on_screen_change_collect_paths(const char * /*id*/, ui::CollectPathsFn collect) {
    static sk::SubscriptionSet s_screen_paths;  // UI-task-local, not on the stack
    s_screen_paths.clear();
    if (collect) {
        // Introspectable screen: subscribe exactly what it shows (+ baseline,
        // which setDesiredPaths unions in). An empty result is fine - the
        // baseline still keeps the link alive.
        collect(s_screen_paths);
    } else {
        // Fullscreen HUD (wind / wind_steer / autopilot) reads sk::data
        // directly; we can't introspect its bindings, so fall back to the FULL
        // legacy path list so it keeps all its data.
        sk::fillFullPathSet(s_screen_paths);
    }
    sk::setDesiredPaths(s_screen_paths);
}

// Recursively tag every descendant of `o` with LV_OBJ_FLAG_EVENT_BUBBLE so a
// gesture starting on any child bubbles up to the screen-root gesture handler.
static void bubble_gestures(lv_obj_t *o) {
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        bubble_gestures(c);
    }
}

static void screen_gesture_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    // Navigation now flows exclusively through touch_task's swipe
    // detector (independent of LVGL's render pipeline so it works even
    // when wind stalls lv_timer_handler for ~2 s). LVGL's gesture path
    // would otherwise fire AGAIN once the stall ends and double-advance.
    // We keep the callback only for diagnostics; it must not navigate.
    g_gesture_suppressed++;
    return;
    // Unreachable - kept for reference until a use case for LVGL-side
    // gestures emerges again.
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_NONE) return;

    const char *dir_name = dir == LV_DIR_LEFT    ? "left"
                           : dir == LV_DIR_RIGHT ? "right"
                           : dir == LV_DIR_TOP   ? "up"
                                                 : "down";
    const char *cur = ui::current_id();

    // Per docs/specs/05 / 06: MOB rescue overlay swallows all navigation
    // swipes - the user must explicitly long-press CLEAR to leave.
    if (g_mob.active) {
        net::logf("[ui] swipe %s suppressed (mob active)", dir_name);
        g_gesture_suppressed++;
        return;
    }

    // Per docs/specs/05: while the WiFi password keyboard is up, horizontal
    // swipes would steal touches meant for the keyboard. Allow vertical
    // (up = settings, down = dashboard) as escape paths.
    if (cur && strcmp(cur, "wifi") == 0 && (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT)) {
        net::logf("[ui] swipe %s suppressed (wifi keyboard active)", dir_name);
        g_gesture_suppressed++;
        return;
    }

    net::logf("[ui] swipe %s screen=%d (%s)", dir_name, ui::current_index(), cur);
    snprintf(g_last_gesture, sizeof(g_last_gesture), "swipe_%s", dir_name);
    g_gesture_count++;

    if (g_demo_timer) {
        demo_stop();
        return;
    }

    if (dir == LV_DIR_LEFT)
        ui::next();
    else if (dir == LV_DIR_RIGHT)
        ui::prev();
    else if (dir == LV_DIR_TOP)
        ui::show_by_id("settings");
    else if (dir == LV_DIR_BOTTOM)
        ui::show(0);
}

// ----- Command handler --------------------------------------------------

static bool handleMainCommand(const String &line) {
#if YEYBOATS_ENABLE_DEMO
    if (line == "demo" || line.startsWith("demo ")) {
        uint32_t period = 3000;
        if (line.length() > 5) period = (uint32_t)line.substring(5).toInt() * 1000;
        demo_start(period);
        return true;
    }
    if (line == "demo-off" || line == "demo off") {
        demo_stop();
        return true;
    }
#endif
#if YEYBOATS_ENABLE_BENCH
    if (line == "fps") {
        fps_overlay_toggle();
        return true;
    }
    if (line == "bench") {
        bench_dump();
        return true;
    }
#ifdef DBG_PERF_COUNTERS
    if (line == "bench-sweep") {
        bench_sweep_start();
        return true;
    }
#endif
    if (line == "irq-probe") {
        irq_probe::arm();
        return true;
    }
    if (line == "irq-probe-dump") {
        irq_probe::dump();
        return true;
    }
    if (line == "irq-probe-stop") {
        irq_probe::disarm();
        return true;
    }
#else
    if (line == "irq-probe" || line == "irq-probe-dump" || line == "irq-probe-stop") {
        return true;
    }
#endif
    if (line == "gt911-config" || line == "touch-config") {
        gt911_dump_config();
        return true;
    }
#if YEYBOATS_ENABLE_TOUCH_CAL_UI
    if (line == "touch-cal" || line == "calibrate") {
        ui::show_by_id("touch_cal");
        return true;
    }
#endif
    if (line == "touch-grid" || line == "grid-cal") {
        ui::show_by_id("touch_grid");
        return true;
    }
    if (line == "config-show") {
        config::UiConfig u = config::ui();
        config::AlarmConfig a = config::alarms();
        config::SignalKConfig sk = config::signalk();
        config::DomainMeta mu = config::meta(config::Domain::Ui);
        config::DomainMeta ma = config::meta(config::Domain::Alarms);
        config::DomainMeta ms = config::meta(config::Domain::SignalK);
        net::logf("[cfg] ui  rev=%u src=%s pending=%d  theme=%s bright=%u fmt=%s",
                  (unsigned)mu.revision, config::source_name(mu.source), mu.persist_pending,
                  config::theme_name(u.theme), (unsigned)u.brightness,
                  config::pos_format_name(u.pos_format));
        net::logf("[cfg] alm rev=%u src=%s pending=%d  depth=%.1fm batt=%.1fV",
                  (unsigned)ma.revision, config::source_name(ma.source), ma.persist_pending,
                  a.depth_alarm_m, a.battery_alarm_v);
        net::logf("[cfg] sk  rev=%u src=%s pending=%d  host=%s:%u%s", (unsigned)ms.revision,
                  config::source_name(ms.source), ms.persist_pending, sk.host, (unsigned)sk.port,
                  sk.token[0] ? " (token)" : "");
        net::logf("[cfg] jobs queued=%u completed=%u failed=%u coalesced=%u",
                  (unsigned)config::persist_jobs_queued(),
                  (unsigned)config::persist_jobs_completed(),
                  (unsigned)config::persist_jobs_failed(), (unsigned)config::coalesced_writes());
        return true;
    }
    if (line == "config-flush") {
        config::flush_pending();
        return true;
    }
    if (line == "latency-reset" || line == "bench-reset") {
#if YEYBOATS_ENABLE_BENCH
        latency::reset_all();
        g_flush_count = 0;
        g_flush_us_total = 0;
        g_flush_us_peak = 0;
        g_flush_px_total = 0;
        g_flush_px_peak = 0;
        g_loop_max_us = 0;
        g_lvgl_max_us = 0;
        g_section_max_us = 0;
        strncpy(g_section_peak_name, "-", sizeof(g_section_peak_name) - 1);
        g_section_peak_name[sizeof(g_section_peak_name) - 1] = 0;
        net::logf("[bench] reset");
#endif
        return true;
    }
    if (line.startsWith("touch-cal-set ")) {
        // Emergency manual calibration: parse six floats a b c d e f.
        // Useful when on-device tap calibration is impractical and we
        // already have a known-good matrix from earlier sample data.
        float a, b, c, d, e, f;
        const char *p = line.c_str() + strlen("touch-cal-set ");
        if (sscanf(p, "%f %f %f %f %f %f", &a, &b, &c, &d, &e, &f) == 6) {
            touch_cal::Matrix m = {a, b, c, d, e, f};
            touch_cal::set(m);
            net::logf("[cal] manual matrix applied");
        } else {
            net::logf("usage: touch-cal-set a b c d e f");
        }
        return true;
    }
    if (line == "touch-cal-reset") {
        // Restore identity matrix in RAM and NVS - effective immediately,
        // no reboot. Use this if a bad calibration locks you out of the
        // UI (taps don't land on the widgets you see).
        touch_cal::reset();
        return true;
    }
    if (line == "wind-refresh" || line.startsWith("wind-refresh ")) {
        String v = line.length() > 13 ? line.substring(13) : String();
        v.trim();
        if (v == "on" || v == "1")
            ui::wind::set_refresh_enabled(true);
        else if (v == "off" || v == "0")
            ui::wind::set_refresh_enabled(false);
        net::logf("[ui] wind-refresh=%s", ui::wind::refresh_enabled() ? "on" : "off");
        return true;
    }
    if (line == "force-invalidate" || line.startsWith("force-invalidate ")) {
        String v = line.length() > 17 ? line.substring(17) : String();
        v.trim();
        if (v == "on" || v == "1")
            g_force_invalidate = true;
        else if (v == "off" || v == "0")
            g_force_invalidate = false;
        net::logf("[ui] force-invalidate=%s", g_force_invalidate ? "on" : "off");
        return true;
    }
    if (line == "mob" || line == "mob-on") {
        mob_trigger();
        return true;
    }
    if (line == "mob-off" || line == "mob-clear") {
        mob_clear();
        return true;
    }
    if (line == "midl-render" || line.startsWith("midl-render ")) {
        // Load the baked MIDL demo doc and render it as a live LVGL screen.
        // Usage: midl-render [screenId]  (default screen id: "midl")
        String sid = "";
        if (line.length() > 12) {
            sid = line.substring(12);
            sid.trim();
        }
        const char *json = midl::demo::SQUARE_480_JSON;
        size_t len = strlen(json);
        void *blob = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!blob) {
            net::logf("[midl-render] PSRAM alloc failed");
            return true;
        }
        memcpy(blob, json, len + 1);  // include NUL so deserializeJson sees a C-string
        app::Command cmd;
        cmd.type = app::CommandType::ConfigApplyMidl;
        cmd.blob = blob;
        cmd.blob_len = len;
        strncpy(cmd.a, sid.c_str(), sizeof(cmd.a) - 1);
        cmd.a[sizeof(cmd.a) - 1] = '\0';
        if (!app::post(cmd)) {
            net::logf("[midl-render] ui queue full");
            heap_caps_free(blob);
        }
        return true;
    }
    if (line == "screen") {
        ui::log_state();
        return true;
    }
    if (line.startsWith("screen ")) {
        String v = line.substring(7);
        v.trim();
        if (v == "next") {
            ui::next();
        } else if (v == "prev") {
            ui::prev();
        } else if (v.length() && isdigit(v[0])) {
            ui::show(v.toInt());
        } else {
            if (!ui::show_by_id(v.c_str())) {
                net::logf("usage: screen <id|next|prev|N>; ids: dashboard wind nav depth status "
                          "steering trip");
                return true;
            }
        }
        return true;
    }
    // Back-compat: old 'view' command (q0..q3 / grid) now maps to first 5 screens
    if (line.startsWith("view ")) {
        String v = line.substring(5);
        v.trim();
        if (v == "grid")
            ui::show(0);
        else if (v == "q0")
            ui::show_by_id("wind");
        else if (v == "q1")
            ui::show_by_id("nav");
        else if (v == "q2")
            ui::show_by_id("depth");
        else if (v == "q3")
            ui::show_by_id("status");
        else
            net::logf("usage: screen <id|next|prev|N>");
        return true;
    }
    if (line == "pos-format") {
        net::logf("pos-format = %s", ui::pos_format_name(ui::pos_format()));
        return true;
    }
    if (line.startsWith("pos-format ")) {
        String fmt = line.substring(11);
        fmt.trim();
        if (fmt == "ddm")
            ui::set_pos_format(ui::PosFormat::DDM);
        else if (fmt == "dd")
            ui::set_pos_format(ui::PosFormat::DD);
        else if (fmt == "dms")
            ui::set_pos_format(ui::PosFormat::DMS);
        else {
            net::logf("usage: pos-format ddm|dd|dms");
            return true;
        }
        net::logf("pos-format -> %s", ui::pos_format_name(ui::pos_format()));
        return true;
    }
    if (line == "trip-reset") {
        ui::trip::reset();
        return true;
    }
    if (line == "thresholds") {
        net::logf("threshold depth=%.1fm battery=%.1fV", ui::depth_alarm_m(),
                  ui::battery_alarm_v());
        return true;
    }
    if (line.startsWith("threshold depth ")) {
        double value = line.substring(16).toDouble();
        ui::set_depth_alarm_m(value);
        net::logf("threshold depth -> %.1fm", ui::depth_alarm_m());
        return true;
    }
    if (line.startsWith("threshold battery ")) {
        double value = line.substring(18).toDouble();
        ui::set_battery_alarm_v(value);
        net::logf("threshold battery -> %.1fV", ui::battery_alarm_v());
        return true;
    }
    if (line == "bright" || line.startsWith("bright ")) {
        if (line == "bright") {
            uint8_t b = ui::brightness();
            net::logf("brightness = %u/255", (unsigned)b);
            return true;
        }
        int b = line.substring(7).toInt();
        ui::set_brightness(b);
        net::logf("brightness -> %u/255", (unsigned)ui::brightness());
        return true;
    }
    if (line.startsWith("theme ")) {
        String v = line.substring(6);
        v.trim();
        if (v == "day") {
            ui::use_day();
            net::logf("[ui] theme -> day (reboot to repaint)");
        } else if (v == "night") {
            ui::use_night();
            net::logf("[ui] theme -> night (reboot to repaint)");
        } else {
            net::logf("usage: theme day|night");
        }
        {
            storage::Namespace p("ui", false);
            p.put_string("theme", v.c_str());
        }
        return true;
    }
    // Control-protocol identity: the controller color (this device's colored
    // frame when it drives another display) and the shared key that gates
    // inbound control. Generic - any device can be a controller (sets color)
    // and/or a target (the key authenticates attaches). Persisted to NVS
    // namespace "proto", keys "color" / "key". The secret is never echoed.
    // Reachable over serial + BLE via the dispatch funnel.
    if (line == "ctl" || line == "ctl status") {
        storage::Namespace p("proto", true);
        std::string color = p.get_string("color", "#00bcd4");
        bool has_key = !p.get_string("key", "").empty();
        net::logf("[ctl] color=%s key=%s", color.c_str(), has_key ? "set" : "none");
        return true;
    }
    if (line.startsWith("ctl color ")) {
        String v = line.substring(10);
        v.trim();
        bool valid = (v.length() == 7 && v[0] == '#');
        for (int i = 1; valid && i < 7; i++)
            valid = isxdigit((unsigned char)v[i]);
        if (!valid) {
            net::logf("usage: ctl color #RRGGBB");
            return true;
        }
        {
            storage::Namespace p("proto", false);
            p.put_string("color", v.c_str());
        }
        net::logf("[ctl] color -> %s", v.c_str());
        return true;
    }
    if (line.startsWith("ctl key ")) {
        String v = line.substring(8);
        v.trim();
        if (v == "clear" || v.startsWith("clear ")) {
            {
                storage::Namespace p("proto", false);
                p.remove("key");
            }
            proto_target::set_key("");
            net::logf("[ctl] key cleared (control open)");
            return true;
        }
        {
            storage::Namespace p("proto", false);
            p.put_string("key", v.c_str());
        }
        proto_target::set_key(v.c_str());
        net::logf("[ctl] key updated");
        return true;
    }
    if (line.startsWith("ctl")) {
        net::logf("usage: ctl [status] | ctl color #RRGGBB | ctl key <secret>|clear");
        return true;
    }
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Runtime encoder calibration for the Waveshare knob (counts/detent +
    // direction invert). Lets the user tune feel without reflashing once the
    // hardware arrives. Reachable over serial + BLE via the dispatch funnel.
    if (line == "knob" || line == "knob status") {
        net::logf("[knob] counts_per_detent=%d invert=%s", knob_input::counts_per_detent(),
                  knob_input::invert() ? "on" : "off");
        return true;
    }
    if (line.startsWith("knob counts ")) {
        String v = line.substring(12);
        v.trim();
        if (!v.length() || !(isdigit(v[0]) || v[0] == '-')) {
            net::logf("usage: knob counts <1-8>");
            return true;
        }
        knob_input::set_counts_per_detent(v.toInt());
        return true;
    }
    if (line.startsWith("knob invert ")) {
        String v = line.substring(12);
        v.trim();
        if (v == "1" || v == "on")
            knob_input::set_invert(true);
        else if (v == "0" || v == "off")
            knob_input::set_invert(false);
        else
            net::logf("usage: knob invert <0|1|on|off>");
        return true;
    }
    if (line.startsWith("knob")) {
        net::logf("usage: knob [status] | knob counts <1-8> | knob invert <0|1|on|off>");
        return true;
    }
#endif
    return false;
}

// The breadcrumb (screen-name chip + position pips) was removed entirely per
// product direction; screens are navigated by swipe with no on-screen indicator.

static void ui_refresh(lv_timer_t *) {
    // Drain queued commands from web/BLE first - they may change the
    // active screen before refresh() runs.
    uint32_t t = micros();
    app::pump();
    note_slow_section("ui:pump", micros() - t);

    // SK-age channel: record how stale sk::data was at the moment the
    // active screen read it. Only sample when SK has produced at least
    // one delta and isn't currently in steady-disconnected state.
    {
        sk::Data d_snap;
        sk::copyData(d_snap);
        if (d_snap.lastUpdateMs) {
            uint32_t age_ms = millis() - d_snap.lastUpdateMs;
            // 1 hour cap so a never-updated reading doesn't pin max.
            if (age_ms < 3600000) {
                latency::record(latency::Channel::SkAge, age_ms * 1000);
            }
        }
    }

    t = micros();
    ui::refresh_current();
    uint32_t refresh_dt = micros() - t;
    note_slow_section("ui:screen", refresh_dt);
#ifdef DBG_PERF_COUNTERS
    g_refresh_us = refresh_dt;
#endif

    t = micros();
    mob_refresh();
    // Reap stale control sessions, then mirror the active set onto the
    // "controlled" frame overlay. set_sessions() dirty-compares, so this is a
    // cheap no-op when nothing changed. Runs on the UI/LVGL task (here).
    proto_target::tick(millis());
    {
        proto::Session ctl_buf[proto::kMaxSessions];
        int ctl_n = proto_target::active_session_snapshot(ctl_buf, proto::kMaxSessions);
        ui::control_frame::set_sessions(ctl_buf, ctl_n);
    }
#if YEYBOATS_ENABLE_STALL_TELEMETRY
    sk::pollStallTelemetry();
#endif
    alarm_check();
    note_slow_section("ui:overlays", micros() - t);

    // Service any pending screenshot request from the web task (snapshot
    // walks LVGL objects, must run on this task).
    t = micros();
    screenshot::serve_pending();
    note_slow_section("ui:screenshot", micros() - t);

    // GT911 INT-line probe (manual via `irq-probe` console command).
    // tick() dumps live counts each ui_refresh and auto-disarms after
    // 10 s. When idle (s_armed == false) the call is a one-line no-op.
    irq_probe::tick();

    // Force a full redraw every cycle. Without this, FPS dropped to 0 on
    // this hardware - LVGL was correctly tracking that "nothing changed"
    // but the panel needs a refresh anyway for time-based animations
    // (needle sweep, uptime label, etc.) to be visible.
    //
    // Runtime-togglable so we can A/B test perf on the wind screen
    // (force-invalidate on|off via console).
    if (g_force_invalidate) {
        g_last_invalidate_us = micros();  // RenderLatency channel
        lv_obj_invalidate(lv_screen_active());
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    puts("\n[boot] ESP32-4848S040 hello-touch");

    // Backlight LEDC is owned by board::set_backlight (spec 13
    // §"Backlight"). main.cpp just asks for "off until display ready".
    board::set_backlight(0);

#ifdef RENDER_DOUBLE_BUFFER
    if (!display_db_init()) {
        puts("[gfx] esp_lcd double-buffer init FAILED");
        while (1)
            delay(1000);
    }
    puts("[gfx] ST7701 + esp_lcd DOUBLE-BUFFER (num_fbs=2) ok");
#else
    if (!gfx->begin()) {
        puts("[gfx] begin FAILED");
        while (1)
            delay(1000);
    }
    gfx->fillScreen(BLACK);
    puts("[gfx] ST7701 RGB panel ok");
#endif

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);
    delay(50);
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Waveshare knob: CST816S capacitive touch (addr 0x15). Secondary input
    // (the menu is encoder-first), so a missing controller is non-fatal.
    g_cst816.begin(TOUCH_INT_ACTIVE_LOW ? FALLING : RISING);
    Wire.beginTransmission(CST816S_ADDRESS);
    bool ok = (Wire.endTransmission() == 0);
    printf("[touch] CST816S probe: %s addr=0x%02X\n", ok ? "ACK" : "no response", CST816S_ADDRESS);
    touch_present = ok;
#else
    Wire.beginTransmission(0x5D);
    bool ok = (Wire.endTransmission() == 0);
    g_gt911_addr = 0x5D;
    if (!ok) {
        Wire.beginTransmission(0x14);
        ok = (Wire.endTransmission() == 0);
        if (ok) g_gt911_addr = 0x14;
    }
    printf("[touch] GT911 probe: %s addr=0x%02X\n", ok ? "ACK" : "no response", g_gt911_addr);
    touch_present = ok;
#endif

    // Load any persisted calibration before the touch task starts so
    // the very first sample is already in screen coordinates.
    touch_cal::setup();

    // Start the dedicated touch polling task on core 0 (LVGL stays on
    // core 1). I2C reads in the LVGL indev callback were stalling render
    // ticks; the task decouples them. Boards that route GT911 INT use it
    // to block while idle, then read coordinates in this task.
    g_touch_mtx = xSemaphoreCreateMutex();
    g_touch_i2c_mtx = xSemaphoreCreateMutex();
    if (touch_present && g_touch_mtx) {
        xTaskCreatePinnedToCore(touch_task, "touch", 4096, nullptr, 2, &g_touch_task, 0);
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
        // CST816S installs its own IRQ handler in begin() and the touch_task
        // drains its event latch via available() at 60 Hz. Do NOT attach the
        // GT911-style touch_irq_isr here -- it would clobber the driver ISR.
        puts("[touch] input mode: poll (CST816S)");
#else
        if (TOUCH_INT >= 0 && g_touch_task) {
#if TOUCH_INT_ACTIVE_LOW
            pinMode(TOUCH_INT, INPUT_PULLUP);
            attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_irq_isr, FALLING);
#else
            pinMode(TOUCH_INT, INPUT_PULLDOWN);
            attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_irq_isr, RISING);
#endif
            g_touch_irq_enabled = true;
            printf("[touch] input mode: irq gpio=%d active_%s\n", TOUCH_INT,
                   TOUCH_INT_ACTIVE_LOW ? "low" : "high");
        } else {
            puts("[touch] input mode: poll");
        }
#endif
    }

    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, disp_flush_cb);

#ifdef RENDER_DOUBLE_BUFFER
    // DOUBLE-BUFFER: hand LVGL the two PANEL framebuffers (from display_db_init)
    // as its two draw buffers in DIRECT mode. LVGL renders dirty areas into the
    // off-screen FB, keeps both FBs in sync itself, and disp_flush_cb flips
    // scan-out on the frame's last area (zero-copy, no blit) -> tear-free + fast.
    // No separate PSRAM draw buffer is allocated; the FBs already live in PSRAM
    // (cfg.flags.fb_in_psram), ~460 KB each.
    lv_display_set_buffers(disp, g_db_fb0, g_db_fb1, (uint32_t)LCD_W * LCD_H * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    puts("[lvgl] esp_lcd DOUBLE-BUFFER num_fbs=2 DIRECT (vsync flip, no blit)");
#else
    // Render path. DIRECT mode renders straight into the RGB panel's scanout
    // framebuffer (gfx->getFramebuffer()), so disp_flush_cb only writes back the
    // dirty rows instead of CPU-copying every tile - fastest, but a SINGLE
    // scanout buffer renders progressively, so large dynamic repaints (steering,
    // full-screen) visibly FLICKER/tear. Measured huge first-paint wins
    // (wind_classic 2.45->0.88s) but the flicker is unacceptable on a marine
    // instrument, so DIRECT is opt-in (-DRENDER_DIRECT_FB) pending a
    // double-buffered/page-flipped panel setup. Default = buffered PARTIAL mode:
    // LVGL renders tiles to an off-screen PSRAM buffer and disp_flush_cb blits
    // each COMPLETE tile in one fast memcpy -> atomic, no flicker. (Internal-SRAM
    // draw buffers would render faster but internal low-water is already ~12 KB
    // with the dedicated LVGL task - the documented WiFi/BLE starvation trap.)
#ifdef RENDER_DIRECT_FB
    uint16_t *fb = gfx->getFramebuffer();
#else
    uint16_t *fb = nullptr;
#endif
    if (fb) {
        g_direct_fb = fb;
        lv_display_set_buffers(disp, fb, NULL, (uint32_t)LCD_W * LCD_H * sizeof(uint16_t),
                               LV_DISPLAY_RENDER_MODE_DIRECT);
        puts("[lvgl] DIRECT render into panel framebuffer (no flush copy)");
    } else {
        // PARTIAL fallback. Buffers in PSRAM (LCD_W*40*2 = ~37.5 KB each):
        // keeping the pair in internal SRAM starved WiFi/lwIP before
        // ("unstable but live"); fall back to internal DMA only if PSRAM fails.
        size_t buf_px = LCD_W * 40;
        uint16_t *buf_a =
            (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        uint16_t *buf_b =
            (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!buf_a || !buf_b) {
            puts("[lvgl] PSRAM buf alloc failed, falling back to internal DMA");
            buf_a = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t),
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            buf_b = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t),
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        }
        lv_display_set_buffers(disp, buf_a, buf_b, buf_px * sizeof(uint16_t),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        puts("[lvgl] PARTIAL render (framebuffer unavailable)");
    }
#endif  // RENDER_DOUBLE_BUFFER

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    // Tune for wet-finger / glove tolerance per docs/specs/06-ui-interactions.md:
    // - long-press 500 ms (up from LVGL's 400 ms) reduces accidental MOB / safety
    //   triggers from glove brushes.
    lv_indev_set_long_press_time(indev, 500);
    // Disable LVGL's own gesture detection. Navigation gestures are now
    // recognized in touch_task (decoupled from the render pipeline so
    // they survive the wind screen's 2-second LVGL stalls). LVGL kept
    // classifying noisy taps as gestures (~50 px finger drift on a
    // capacitive panel) and swallowing the CLICKED event for settings
    // buttons. Raising the per-tick velocity threshold to 255 px makes
    // LVGL's gesture path practically unreachable without disturbing
    // its tap/long-press handling.
    lv_indev_set_gesture_min_velocity(indev, 255);
    // Capacitive-touch finger contact drifts 20-80 px during a normal
    // "tap" on this panel. Default scroll_limit=10 made every such tap
    // a drag and the button never fired CLICKED. Match scroll_limit to
    // SWIPE_MIN_PX so the only motion bands are:
    //
    //   0 .. SWIPE_MIN_PX-1  -> LVGL CLICKED on release (tap)
    //   SWIPE_MIN_PX ..      -> touch_task posts ShowScreen (swipe)
    //
    // No dead zone between them. Sliders use their own PRESSING handler
    // so this does not affect drag-to-set behavior.
    lv_indev_set_scroll_limit(indev, SWIPE_MIN_PX);

    // Load theme + position format prefs
    {
        storage::Namespace p("ui", true);
        String themePref = String(p.get_string("theme", "night").c_str());
        ui::set_pos_format((ui::PosFormat)p.get_u8("pos_fmt", (uint8_t)ui::PosFormat::DDM));
        if (themePref == "day")
            ui::use_day();
        else
            ui::use_night();
    }

    // Each screen is a DETACHED screen object (parent = NULL). Screen
    // manager swaps via lv_screen_load, so only the active screen lives
    // in the render tree at any time. Possible now that LVGL pool sits in
    // PSRAM (8 MB) - all 10 screen trees fit with room to spare.
    // Boot-time eager build for dashboard (first frame after boot must
    // show something). All other screens are lazy: their LVGL trees only
    // get allocated when the operator navigates to them, freeing boot heap
    // for SK websocket buffers and OTA. Once visited, the root is cached
    // for the session - no rebuild on subsequent visits.
#ifndef YEYBOATS_MIDL_ONLY
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Waveshare knob: dedicated round views only. The autopilot HUD is the
    // first/home view; the rotary menu (knob_ui) drives mode + view switching.
    ui::register_screen_lazy("ap_hud", "Autopilot", ui::ap_hud::build, ui::ap_hud::refresh, false);
    ui::register_screen_lazy("knob_compass", "Compass", ui::knob_compass::build,
                             ui::knob_compass::refresh, false);
    ui::register_screen_lazy("knob_wind", "Wind", ui::knob_wind::build, ui::knob_wind::refresh,
                             false);
    ui::register_screen_lazy("knob_big", "Big", ui::knob_big::build, ui::knob_big::refresh, false);
    knob_ui::setup();  // inits model + seeds knob_remote registry + builds overlay
    ui::show_by_id("ap_hud");
#else
    ui::Screen dash = {};
    dash.id = "dashboard";
    dash.title = "Dashboard";
    dash.root = ui::dashboard::build(NULL);
    dash.refresh = ui::dashboard::refresh;
    ui::register_screen(dash);

    // Hidden: not in the swipe rotation; reached only by tapping a dashboard
    // tile (and dismissed by tapping it or swiping down).
    ui::register_screen_lazy("zoom", "Zoom", ui::zoom::build, ui::zoom::refresh, true);
    ui::register_screen_lazy("wind", "Wind", ui::wind::build, ui::wind::refresh, false);
    ui::register_screen_lazy("wind_classic", "Wind (classic)", ui::wind_classic::build,
                             ui::wind_classic::refresh, false);
    ui::register_screen_lazy("wind_steer", "Wind Steer", ui::wind_steer::build,
                             ui::wind_steer::refresh, false);
    ui::register_screen_lazy("nav", "Nav", ui::nav::build, ui::nav::refresh, false);
    ui::register_screen_lazy("depth", "Depth", ui::depth::build, ui::depth::refresh, false);
    ui::register_screen_lazy("steering", "Steering", ui::steering::build, ui::steering::refresh,
                             false);
    ui::register_screen_lazy("route", "Route", ui::route::build, ui::route::refresh, false);
    ui::register_screen_lazy("autopilot", "Autopilot", ui::autopilot::build, ui::autopilot::refresh,
                             false);
    ui::register_screen_lazy("trip", "Trip", ui::trip::build, ui::trip::refresh, false);
    ui::register_screen_lazy("status", "System", ui::status_panel::build, ui::status_panel::refresh,
                             false);
    ui::register_screen_lazy("wifi", "WiFi Setup", ui::wifi_setup::build, ui::wifi_setup::refresh,
                             true);
    ui::register_screen_lazy("settings", "Settings", ui::settings::build, ui::settings::refresh,
                             true);
#if YEYBOATS_ENABLE_TOUCH_CAL_UI
    ui::register_screen_lazy("touch_cal", "Touch Cal", ui::touch_cal_screen::build,
                             ui::touch_cal_screen::refresh, true);
#endif
    ui::register_screen_lazy("touch_grid", "Touch Grid", ui::touch_grid_screen::build,
                             ui::touch_grid_screen::refresh, true);
    ui::register_screen_lazy("demo_grid", "Demo Grid", ui::demo_grid::build, ui::demo_grid::refresh,
                             true);
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8 (else: Sunton screen registration)
#else   // YEYBOATS_MIDL_ONLY
    {
        // MIDL-only boot: the device's UI is the baked MIDL demo doc.
        // setup() runs on the LVGL/loop task, so building LVGL here is safe.
        // apply_all() registers ALL screens in the doc (Dashboard/Navigation/
        // Speed) so `screen <id|next|prev>` navigation works; it also shows the
        // settings.defaultScreen ("dash"), so no explicit show_by_id is needed.
        JsonDocument midlDoc(&yeyboats::psram_json);  // pool in PSRAM, not internal heap
        if (deserializeJson(midlDoc, midl::demo::SQUARE_480_JSON) == DeserializationError::Ok) {
            size_t n = midl::render::apply_all(midlDoc.as<JsonVariantConst>());
            net::logf("[midl-only] apply_all registered %u screen(s)", (unsigned)n);
        } else {
            net::logf("[midl-only] baked doc parse failed");
        }
    }
#endif  // YEYBOATS_MIDL_ONLY

    // Attach the gesture handler to EVERY screen root via a post-build
    // hook. LVGL routes LV_EVENT_GESTURE to the currently loaded screen
    // (or the widget the touch landed on), not to lv_layer_top - so
    // without per-screen handlers swipes that started on empty screen
    // area silently dropped. set_post_build_cb retroactively fires for
    // already-built (eager) screens AND fires for each lazy screen
    // when its root is built on first activation.
    ui::set_post_build_cb([](lv_obj_t *root, const char * /*id*/) {
        lv_obj_add_event_cb(root, screen_gesture_handler, LV_EVENT_GESTURE, NULL);
        // LVGL delivers LV_EVENT_GESTURE to the widget the touch landed on; it
        // only reaches the root handler above if intermediate children carry
        // LV_OBJ_FLAG_EVENT_BUBBLE. Hand-built screens (wind_classic,
        // wind_steer, autopilot, ...) have dial/tile/button children without
        // that flag, so a swipe starting on a child never bubbles and those
        // screens become unreachable/un-leavable by swipe. Tag every
        // descendant so the gesture always bubbles to root. (CLICK also
        // bubbles, but root handles only LV_EVENT_GESTURE and a swipe does not
        // fire a button CLICK, so the buttons' own click handlers are intact.)
        bubble_gestures(root);
    });

    // Slice 3: when the active screen changes, compute the paths it shows and
    // hand them to the SK subscription manager. Screens that can't be
    // introspected (fullscreen HUDs reading sk::data directly; collect == NULL)
    // get the full baseline so they keep all their data. The scratch
    // SubscriptionSet is function-static (UI task runs this serially) - never a
    // stack local: it's ~5 KB and would overflow the loopTask stack
    // (CLAUDE.md large-struct-on-stack trap).
    ui::set_screen_change_cb(on_screen_change_collect_paths);

    // Global overlays + gestures also live on lv_layer_top() so they
    // survive screen swaps and catch gestures that landed on overlay
    // children (MOB button etc.).
    lv_obj_t *top = lv_layer_top();
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top, screen_gesture_handler, LV_EVENT_GESTURE, NULL);

    mob_build(top);
    alarms_build(top);
    // Per-controller "controlled" frame (Phase 3.2): stacked colored borders +
    // name-pill on lv_layer_top(), refreshed from proto_target in ui_refresh.
    // Self-attaches to lv_layer_top(); the parent arg is ignored.
    ui::control_frame::build(nullptr);

    // (initial screen selection happens AFTER net::setup() runs below;
    // doing it here would always see WiFi as down because we haven't
    // tried to join yet.)

    // Restore last-known brightness from NVS (default 200/255 ~ 78%).
    // LEDC is owned by board::set_backlight (spec 13 §"Backlight").
    {
        uint8_t b = ui::brightness();
        board::set_backlight(b);
        printf("[bl] brightness %u/255\n", (unsigned)b);
    }
    puts("[boot] ready");

    // App event queue must exist before any task that wants to post.
    app::setup();
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    knob_input::setup();  // encoder task posts to app queue; must run after app::setup()
#endif
    screenshot::setup();
    // Load persisted UI/Alarm/SignalK config into the RAM-first config
    // owner before any module that reads those values. Replaces direct
    // Preferences reads scattered in ui_data, screen_settings, etc.
    config::setup();
    // net::setup() returns immediately now (Phase 4). The wifi manager
    // task posts ShowScreen("wifi") when it falls into AP mode; otherwise
    // we stay on the already-loaded dashboard.
    net::setup();
    web::setup();

    layout::load_default();
    sk::setup("", 3000);
    nmea_wifi::setup();
    nmea2000::setup();
    device_identity::setup();
    beeper::setup();
    autopilot::setup();
    manager::setup();

    lv_timer_create(ui_refresh, 200, NULL);
    lv_timer_create(fps_tick, 1000, NULL);

    net::setExtraCommandHandler(handleMainCommand);
    ui::log_state();

    // Start the dedicated LVGL pump LAST: all display/screen/timer init above
    // ran single-threaded on loopTask; from here lv_* lives only on g_lvgl_task.
    // Core 1 (same as loopTask/Arduino) so heavy renders never land on core 0's
    // WiFi/BLE stack; priority 1 to time-share cooperatively with loop(). 10 KB
    // stack: measured peak use is ~5.7 KB (uxTaskGetStackHighWaterMark), and a
    // smaller stack keeps internal-SRAM low-water out of the WiFi-starvation zone.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 10240, nullptr, 1, &g_lvgl_task, 1);
}

static String serial_line;

static void pollSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            serial_line.trim();
            if (serial_line.length()) {
                if (!net::handleSerialCommand(serial_line) &&
                    !sk::handleSerialCommand(serial_line) &&
                    !manager::handleSerialCommand(serial_line) &&
                    !layout::handleSerialCommand(serial_line)) {
                    handleMainCommand(serial_line);
                }
            }
            serial_line = "";
        } else if (serial_line.length() < 200) {
            serial_line += c;
        }
    }
}

// Dedicated LVGL pump. Runs lv_timer_handler (render + ui_refresh + app::pump +
// touch indev + screenshot) on its own core-1 task so a slow first-paint stall
// no longer blocks net/web/serial in loop() and vice versa. LVGL is single-
// threaded: ALL lv_* calls must stay on this task; the rest of the system
// enqueues work via app::Command. Generous 16 KB stack - the SW rasterizer and
// app::pump can go deep (see "never build a large struct on a task stack").
// Set true around a firmware OTA: the LVGL pump (flash-resident code that
// churns the PSRAM framebuffer) must go quiet during the sustained 2 MB
// Update.write, or it intermittently hangs the device on this precompiled
// arduino-esp32 build (flash-cache-disabled window vs. cached access — the
// CLAUDE.md flash-cache hazard). The panel keeps showing the last frame.
volatile bool g_ui_paused = false;
void app_pause_ui(bool paused) {
    g_ui_paused = paused;
}

static void lvgl_task(void *) {
    for (;;) {
        if (g_ui_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t t = micros();
        lv_timer_handler();
        uint32_t dt = micros() - t;
        if (dt > g_lvgl_max_us) g_lvgl_max_us = dt;
        note_slow_section("lvgl", dt);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void loop() {
    uint32_t t_loop = micros();

    // lv_timer_handler now runs on g_lvgl_task (started at end of setup), not
    // here - loop() owns connectivity/serial only.
    uint32_t t_section = micros();
    net::loop();
    note_slow_section("net", micros() - t_section);

    // sk runs on its own task (sk_task on core 0). sk::loop() is now
    // a no-op kept for ABI compatibility; we don't time it.

    t_section = micros();
    web::loop();
    note_slow_section("web", micros() - t_section);

    t_section = micros();
    pollSerialCommands();
    note_slow_section("serial", micros() - t_section);

    uint32_t dt_loop = micros() - t_loop;
    if (dt_loop > g_loop_max_us) g_loop_max_us = dt_loop;
    g_loop_last_ms = millis();
    delay(5);
}
