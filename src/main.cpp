#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "screens.h"
#include "web.h"

#include <Preferences.h>
#include <math.h>

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

static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK, RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4, RGB_G0, RGB_G1,
    RGB_G2, RGB_G3, RGB_G4, RGB_G5, RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4, 1 /* hsync_polarity */,
    10, 8, 50, 1 /* vsync_polarity */, 10, 8, 20, 1 /* pclk_active_neg */,
    12000000L /* prefer_speed 12MHz */, false /* useBigEndian */);

static Arduino_RGB_Display *gfx =
    new Arduino_RGB_Display(LCD_W, LCD_H, rgbpanel, 0 /* rotation */, true /* auto_flush */, bus,
                            -1 /* RST */, st7701_4848S040_init, sizeof(st7701_4848S040_init));

static bool touch_present = false;

// FPS benchmark counters (updated from disp_flush_cb).
static volatile uint32_t g_flush_count = 0;
static volatile uint32_t g_flush_us_total = 0;
static volatile uint32_t g_flush_us_peak = 0;

static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
    uint32_t t0 = micros();
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(d);
    uint32_t dt = micros() - t0;
    g_flush_count++;
    g_flush_us_total += dt;
    if (dt > g_flush_us_peak) g_flush_us_peak = dt;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    static int16_t last_x = -1, last_y = -1;

    if (!touch_present) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    Wire.beginTransmission(0x5D);
    Wire.write(0x81);
    Wire.write(0x4E);
    if (Wire.endTransmission(false) != 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    Wire.requestFrom(0x5D, 1);
    if (!Wire.available()) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    uint8_t status = Wire.read();
    uint8_t pts = status & 0x0F;
    if ((status & 0x80) && pts > 0) {
        Wire.beginTransmission(0x5D);
        Wire.write(0x81);
        Wire.write(0x50);
        Wire.endTransmission(false);
        Wire.requestFrom(0x5D, 6);
        if (Wire.available() >= 6) {
            (void)Wire.read();
            // Empirically verified: GT911 on this Sunton 4848S040 panel
            // returns coord bytes high-byte-first (big-endian) within each
            // 16-bit field, contrary to most GT911 references.
            uint8_t xh = Wire.read();
            uint8_t xl = Wire.read();
            uint8_t yh = Wire.read();
            uint8_t yl = Wire.read();
            uint16_t x = ((uint16_t)xh << 8) | xl;
            uint16_t y = ((uint16_t)yh << 8) | yl;
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
            if (last_state == LV_INDEV_STATE_RELEASED || abs((int)x - last_x) > 20 ||
                abs((int)y - last_y) > 20) {
                net::logf("[touch] DOWN raw=(%d,%d) pts=%d", x, y, pts);
                last_x = x;
                last_y = y;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    if (last_state == LV_INDEV_STATE_PRESSED && data->state == LV_INDEV_STATE_RELEASED) {
        net::logf("[touch] UP   raw=(%d,%d)", last_x, last_y);
    }
    last_state = data->state;
    Wire.beginTransmission(0x5D);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0x00);
    Wire.endTransmission();
}

static uint32_t lv_tick_cb(void) { return millis(); }

// ----- Global overlays (visible on every screen) -------------------------
// MOB button + rescue overlay
static struct {
    bool active = false;
    double lat = NAN, lon = NAN;
    uint32_t trigger_ms = 0;
} g_mob;
static lv_obj_t *mob_button = nullptr;
static lv_obj_t *mob_view = nullptr;
static lv_obj_t *mob_lbl_dist, *mob_lbl_brg, *mob_lbl_back, *mob_lbl_elapsed;

// Alarm banner
enum AlarmId { ALARM_NONE = 0, ALARM_DEPTH_SHALLOW, ALARM_SK_STALLED, ALARM_BATT_LOW };
static AlarmId g_alarm = ALARM_NONE;
static lv_obj_t *alarm_banner = nullptr;
static lv_obj_t *alarm_label = nullptr;
static const double ALARM_DEPTH_M = 3.0;
static const double ALARM_BATT_V = 11.5;

// FPS overlay
static lv_obj_t *g_fps_overlay = nullptr;
static float g_fps = 0.0f;
static uint32_t g_fps_peak_us = 0;
static float g_fps_avg_us = 0.0f;

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
    while (b < 0) b += 360;
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

static void mob_btn_evt(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) mob_trigger();
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
        while (back >= 360) back -= 360;
        snprintf(buf, sizeof(buf), "return %03.0f\xC2\xB0", back);
        lv_label_set_text(mob_lbl_back, buf);
    }
    uint32_t s = (millis() - g_mob.trigger_ms) / 1000;
    snprintf(buf, sizeof(buf), "elapsed %02lu:%02lu", (unsigned long)(s / 60),
             (unsigned long)(s % 60));
    lv_label_set_text(mob_lbl_elapsed, buf);
}

static void mob_build(lv_obj_t *scr) {
    mob_button = lv_obj_create(scr);
    lv_obj_set_size(mob_button, 56, 56);
    lv_obj_align(mob_button, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(mob_button, lv_color_hex(ui::theme.alarm), 0);
    lv_obj_set_style_border_color(mob_button, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(mob_button, 2, 0);
    lv_obj_set_style_radius(mob_button, 28, 0);
    lv_obj_set_style_pad_all(mob_button, 0, 0);
    lv_obj_clear_flag(mob_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mob_button, mob_btn_evt, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(mob_button);
    lv_label_set_text(btn_lbl, "MOB");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_lbl);

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
    lv_obj_set_size(alarm_banner, 360, 36);
    lv_obj_align(alarm_banner, LV_ALIGN_BOTTOM_MID, 0, -4);
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
}

static void alarm_set(AlarmId id, const char *msg) {
    if (g_alarm == id) return;
    g_alarm = id;
    if (!alarm_banner) return;
    if (id == ALARM_NONE) {
        lv_obj_add_flag(alarm_banner, LV_OBJ_FLAG_HIDDEN);
        net::logf("[alarm] cleared");
    } else {
        lv_label_set_text(alarm_label, msg);
        lv_obj_clear_flag(alarm_banner, LV_OBJ_FLAG_HIDDEN);
        net::logf("[alarm] %s", msg);
    }
}

static void alarm_check() {
    if (!isnan(sk::data.depth) && sk::data.depth > 0 && sk::data.depth < ALARM_DEPTH_M) {
        alarm_set(ALARM_DEPTH_SHALLOW, "SHALLOW WATER");
        return;
    }
    if (sk::connectionStatus() == "stalled") {
        alarm_set(ALARM_SK_STALLED, "SIGNALK STALLED");
        return;
    }
    if (!isnan(sk::data.battVoltage) && sk::data.battVoltage < ALARM_BATT_V) {
        alarm_set(ALARM_BATT_LOW, "BATTERY LOW");
        return;
    }
    alarm_set(ALARM_NONE, "");
}

// ----- Demo / fps -------------------------------------------------------

static void demo_tick(lv_timer_t *) { ui::next(); }

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

static void bench_dump() {
    size_t heap_free = esp_get_free_heap_size();
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    net::logf("[bench] fps=%.1f Hz", g_fps);
    net::logf("[bench] flush avg=%.0f us  peak=%lu us", g_fps_avg_us, (unsigned long)g_fps_peak_us);
    net::logf("[bench] heap free=%u KB", (unsigned)(heap_free / 1024));
    net::logf("[bench] psram free=%u / %u KB", (unsigned)(psram_free / 1024),
              (unsigned)(psram_total / 1024));
    net::logf("[bench] sk: %s", sk::connectionStatus().c_str());
}

// ----- Gesture handler --------------------------------------------------

static void screen_gesture_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    if (g_mob.active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_NONE) return;

    const char *dir_name = dir == LV_DIR_LEFT    ? "left"
                           : dir == LV_DIR_RIGHT ? "right"
                           : dir == LV_DIR_TOP   ? "up"
                                                 : "down";
    net::logf("[ui] swipe %s screen=%d (%s)", dir_name, ui::current_index(), ui::current_id());

    if (g_demo_timer) {
        demo_stop();
        return;
    }

    if (dir == LV_DIR_LEFT) ui::next();
    else if (dir == LV_DIR_RIGHT) ui::prev();
    else if (dir == LV_DIR_BOTTOM) ui::show(0);
}

// ----- Command handler --------------------------------------------------

static bool handleMainCommand(const String &line) {
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
    if (line == "fps") {
        fps_overlay_toggle();
        return true;
    }
    if (line == "bench") {
        bench_dump();
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
                net::logf("usage: screen <id|next|prev|N>; ids: dashboard wind nav depth status steering trip");
                return true;
            }
        }
        return true;
    }
    // Back-compat: old 'view' command (q0..q3 / grid) now maps to first 5 screens
    if (line.startsWith("view ")) {
        String v = line.substring(5);
        v.trim();
        if (v == "grid") ui::show(0);
        else if (v == "q0") ui::show_by_id("wind");
        else if (v == "q1") ui::show_by_id("nav");
        else if (v == "q2") ui::show_by_id("depth");
        else if (v == "q3") ui::show_by_id("status");
        else net::logf("usage: screen <id|next|prev|N>");
        return true;
    }
    if (line == "pos-format") {
        net::logf("pos-format = %s", ui::pos_format_name(ui::pos_format()));
        return true;
    }
    if (line.startsWith("pos-format ")) {
        String fmt = line.substring(11);
        fmt.trim();
        if (fmt == "ddm") ui::set_pos_format(ui::PosFormat::DDM);
        else if (fmt == "dd") ui::set_pos_format(ui::PosFormat::DD);
        else if (fmt == "dms") ui::set_pos_format(ui::PosFormat::DMS);
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
        Preferences p;
        p.begin("ui", false);
        p.putString("theme", v);
        p.end();
        return true;
    }
    return false;
}

// ----- breadcrumb (current screen indicator) -----------------------------
// Small chip top-center: "Wind 2/9" plus a row of pips below.
static lv_obj_t *bc_label = nullptr;
static lv_obj_t *bc_pips = nullptr;

void breadcrumb_build(lv_obj_t *scr) {
    bc_label = lv_label_create(scr);
    lv_label_set_text(bc_label, "");
    lv_obj_set_style_text_font(bc_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bc_label, lv_color_hex(ui::theme.fg), 0);
    lv_obj_set_style_bg_color(bc_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bc_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(bc_label, 8, 0);
    lv_obj_set_style_pad_ver(bc_label, 2, 0);
    lv_obj_set_style_radius(bc_label, 8, 0);
    lv_obj_align(bc_label, LV_ALIGN_TOP_MID, 0, 2);

    bc_pips = lv_obj_create(scr);
    lv_obj_set_size(bc_pips, 200, 8);
    lv_obj_align(bc_pips, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_opa(bc_pips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bc_pips, 0, 0);
    lv_obj_set_style_pad_all(bc_pips, 0, 0);
    lv_obj_clear_flag(bc_pips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bc_pips, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(bc_pips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(bc_pips, 4, 0);
    lv_obj_set_flex_align(bc_pips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static void breadcrumb_refresh() {
    if (!bc_label) return;
    // Build the dot strip lazily: visible screens get a pip, current one is brighter.
    static int last_index = -2;
    static size_t last_count = 0;
    int idx = ui::current_index();
    size_t cnt = ui::screen_count();
    if (idx == last_index && cnt == last_count) return;  // unchanged
    last_index = idx;
    last_count = cnt;

    char buf[32];
    snprintf(buf, sizeof(buf), "%s  %d/%u", ui::current_title(), idx + 1, (unsigned)cnt);
    lv_label_set_text(bc_label, buf);

    lv_obj_clean(bc_pips);
    for (size_t i = 0; i < cnt; ++i) {
        if (ui::is_hidden(i) && (int)i != idx) continue;
        lv_obj_t *p = lv_obj_create(bc_pips);
        lv_obj_set_size(p, 8, 8);
        bool active = ((int)i == idx);
        lv_obj_set_style_bg_color(
            p, lv_color_hex(active ? ui::theme.accent : ui::theme.fg_dim), 0);
        lv_obj_set_style_bg_opa(p, active ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_radius(p, 4, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void ui_refresh(lv_timer_t *) {
    ui::refresh_current();
    breadcrumb_refresh();
    mob_refresh();
    alarm_check();
    // Force a full redraw every cycle. Without this, FPS dropped to 0 on
    // this hardware - LVGL was correctly tracking that "nothing changed"
    // but the panel needs a refresh anyway for time-based animations
    // (needle sweep, uptime label, etc.) to be visible.
    lv_obj_invalidate(lv_screen_active());
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] ESP32-4848S040 hello-touch");

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);

    if (!gfx->begin()) {
        Serial.println("[gfx] begin FAILED");
        while (1) delay(1000);
    }
    gfx->fillScreen(BLACK);
    Serial.println("[gfx] ST7701 RGB panel ok");

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);
    delay(50);
    Wire.beginTransmission(0x5D);
    bool ok = (Wire.endTransmission() == 0);
    if (!ok) {
        Wire.beginTransmission(0x14);
        ok = (Wire.endTransmission() == 0);
    }
    Serial.printf("[touch] GT911 probe: %s\n", ok ? "ACK" : "no response");
    touch_present = ok;

    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    size_t buf_px = LCD_W * 40;
    uint16_t *buf_a = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint16_t *buf_b = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf_a || !buf_b) {
        Serial.println("[lvgl] DMA buf alloc failed, falling back to PSRAM");
        buf_a = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        buf_b = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf_a, buf_b, buf_px * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    // Load theme + position format prefs
    {
        Preferences p;
        p.begin("ui", true);
        String themePref = p.getString("theme", "night");
        ui::set_pos_format((ui::PosFormat)p.getUChar("pos_fmt", (uint8_t)ui::PosFormat::DDM));
        p.end();
        if (themePref == "day") ui::use_day();
        else ui::use_night();
    }

    // Each screen is a DETACHED screen object (parent = NULL). Screen
    // manager swaps via lv_screen_load, so only the active screen lives
    // in the render tree at any time. Possible now that LVGL pool sits in
    // PSRAM (8 MB) - all 10 screen trees fit with room to spare.
    ui::register_screen({"dashboard", "Dashboard",  ui::dashboard::build(NULL),     ui::dashboard::refresh,    false});
    ui::register_screen({"wind",      "Wind",       ui::wind::build(NULL),          ui::wind::refresh,         false});
    ui::register_screen({"nav",       "Nav",        ui::nav::build(NULL),           ui::nav::refresh,          false});
    ui::register_screen({"depth",     "Depth",      ui::depth::build(NULL),         ui::depth::refresh,        false});
    ui::register_screen({"steering",  "Steering",   ui::steering::build(NULL),      ui::steering::refresh,     false});
    ui::register_screen({"route",     "Route",      ui::route::build(NULL),         ui::route::refresh,        false});
    ui::register_screen({"autopilot", "Autopilot",  ui::autopilot::build(NULL),     ui::autopilot::refresh,    false});
    ui::register_screen({"trip",      "Trip",       ui::trip::build(NULL),          ui::trip::refresh,         false});
    ui::register_screen({"status",    "System",     ui::status_panel::build(NULL),  ui::status_panel::refresh, false});
    ui::register_screen({"wifi",      "WiFi Setup", ui::wifi_setup::build(NULL),    ui::wifi_setup::refresh,   true});

    // Global overlays + gestures live on lv_layer_top() so they survive
    // screen swaps without re-parenting.
    lv_obj_t *top = lv_layer_top();
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top, screen_gesture_handler, LV_EVENT_GESTURE, NULL);

    mob_build(top);
    alarms_build(top);
    breadcrumb_build(top);

    if (!net::wifiUp()) {
        ui::show_by_id("wifi");
    }

    digitalWrite(LCD_BL, HIGH);
    Serial.println("[boot] ready");

    net::setup();
    net::logf("[net] up - ip=%s", net::ipString().c_str());
    web::setup();

    layout::load_default();
    sk::setup("", 3000);

    lv_timer_create(ui_refresh, 200, NULL);
    lv_timer_create(fps_tick, 1000, NULL);

    net::setExtraCommandHandler(handleMainCommand);
    ui::log_state();
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

void loop() {
    lv_timer_handler();
    net::loop();
    sk::loop();
    web::loop();
    pollSerialCommands();
    delay(5);
}
