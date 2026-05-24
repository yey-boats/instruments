#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
#include "layout_loader.h"
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
    // Poll GT911 directly over I2C; INT/RST are not wired on this board.
    Wire.beginTransmission(0x5D);
    Wire.write(0x81);
    Wire.write(0x4E);  // GT911 status register
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
            (void)Wire.read();  // track id
            // Empirically verified: GT911 on this Sunton 4848S040 panel
            // returns coord bytes high-byte-first (big-endian) within
            // each 16-bit field, contrary to most GT911 references.
            // Reading them as little-endian gave raw values in the
            // 30000+ range. Tap-tested at known positions to confirm.
            uint8_t xh = Wire.read();
            uint8_t xl = Wire.read();
            uint8_t yh = Wire.read();
            uint8_t yl = Wire.read();
            uint16_t x = ((uint16_t)xh << 8) | xl;
            uint16_t y = ((uint16_t)yh << 8) | yl;
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
            if (last_state == LV_INDEV_STATE_RELEASED ||
                abs((int)x - last_x) > 20 || abs((int)y - last_y) > 20) {
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
    // Clear status register
    Wire.beginTransmission(0x5D);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0x00);
    Wire.endTransmission();
}

static uint32_t lv_tick_cb(void) {
    return millis();
}

// Marine dashboard: 2x2 quadrants on 480x480, each ~232x232.
static lv_obj_t *lbl_aws, *lbl_awa;
static lv_obj_t *needle;
static lv_obj_t *lbl_sog, *lbl_cog, *lbl_hdg, *lbl_pos;
static lv_obj_t *lbl_depth, *lbl_temp;
static lv_obj_t *lbl_batt, *lbl_status, *lbl_ip, *lbl_rssi;
static lv_obj_t *quadrants[4];  // saved q1..q4 for demo focus / future fullscreen

// Demo mode state - cycles through quadrants for video capture.
static int g_demo_state = -1;  // -1 = off, 0..3 = focused quadrant, 4 = grid pause
static lv_timer_t *g_demo_timer = nullptr;
static uint32_t g_demo_period_ms = 3000;
static lv_obj_t *g_demo_badge = nullptr;

// Triple-tap state and current focused quadrant (-1 = grid view).
static int g_focused_quad = -1;
static uint32_t g_tap_times[3] = {0, 0, 0};

// MOB (Man Overboard) state - captured GPS at trigger time.
static struct {
    bool active = false;
    double lat = NAN, lon = NAN;
    uint32_t trigger_ms = 0;
} g_mob;
static lv_obj_t *mob_button = nullptr;
static lv_obj_t *mob_view = nullptr;
static lv_obj_t *mob_lbl_dist, *mob_lbl_brg, *mob_lbl_back, *mob_lbl_elapsed;

// Position formatting (configurable via `pos-format` console cmd, NVS-persisted)
enum PosFormat : uint8_t {
    POS_DDM = 0,  // 41°23.106'N  002°10.404'E   - marine standard
    POS_DD = 1,   // 41.3851°N    2.1734°E       - decimal degrees
    POS_DMS = 2,  // 41°23'06.4"N  2°10'24.2"E   - deg/min/sec
};
static PosFormat g_pos_format = POS_DDM;

static const char *pos_format_name(PosFormat f) {
    switch (f) {
    case POS_DDM:
        return "ddm";
    case POS_DD:
        return "dd";
    case POS_DMS:
        return "dms";
    }
    return "?";
}

// Renders into buf as two newline-separated lines (lat, lon).
static void format_position(double lat, double lon, char *buf, size_t cap) {
    char ns = lat >= 0 ? 'N' : 'S';
    char ew = lon >= 0 ? 'E' : 'W';
    double la = fabs(lat);
    double lo = fabs(lon);
    switch (g_pos_format) {
    case POS_DDM: {
        int la_d = (int)la;
        double la_m = (la - la_d) * 60.0;
        int lo_d = (int)lo;
        double lo_m = (lo - lo_d) * 60.0;
        snprintf(buf, cap, "%02d°%06.3f'%c\n%03d°%06.3f'%c", la_d, la_m, ns, lo_d, lo_m, ew);
        break;
    }
    case POS_DD:
        snprintf(buf, cap, "%.4f°%c\n%.4f°%c", la, ns, lo, ew);
        break;
    case POS_DMS: {
        int la_d = (int)la;
        double la_r = (la - la_d) * 60.0;
        int la_m = (int)la_r;
        double la_s = (la_r - la_m) * 60.0;
        int lo_d = (int)lo;
        double lo_r = (lo - lo_d) * 60.0;
        int lo_m = (int)lo_r;
        double lo_s = (lo_r - lo_m) * 60.0;
        snprintf(buf, cap, "%d°%02d'%04.1f\"%c\n%d°%02d'%04.1f\"%c", la_d, la_m, la_s, ns, lo_d,
                 lo_m, lo_s, ew);
        break;
    }
    }
}

// FPS overlay state
static lv_obj_t *g_fps_overlay = nullptr;
static float g_fps = 0.0f;
static uint32_t g_fps_peak_us = 0;
static float g_fps_avg_us = 0.0f;

// Alarms (single-slot, highest-priority condition shown)
enum AlarmId { ALARM_NONE = 0, ALARM_DEPTH_SHALLOW, ALARM_SK_STALLED, ALARM_BATT_LOW };
static AlarmId g_alarm = ALARM_NONE;
static lv_obj_t *alarm_banner = nullptr;
static lv_obj_t *alarm_label = nullptr;

// Thresholds (TODO: configurable via #7 server-managed layout config)
static const double ALARM_DEPTH_M = 3.0;
static const double ALARM_BATT_V = 11.5;

// Forward decls (definitions live below build_ui).
static void screen_tap_handler(lv_event_t *e);
static void screen_gesture_handler(lv_event_t *e);
static void mob_build(lv_obj_t *scr);
static void alarms_build(lv_obj_t *scr);

static lv_obj_t *make_quadrant(lv_obj_t *parent, int qx, int qy, const char *header) {
    lv_obj_t *q = lv_obj_create(parent);
    lv_obj_set_size(q, 232, 232);
    lv_obj_set_pos(q, qx * 240 + 4, qy * 240 + 4);
    lv_obj_set_style_bg_color(q, lv_color_hex(0x0a2540), 0);
    lv_obj_set_style_border_color(q, lv_color_hex(0x223a55), 0);
    lv_obj_set_style_border_width(q, 1, 0);
    lv_obj_set_style_radius(q, 8, 0);
    lv_obj_set_style_pad_all(q, 8, 0);
    lv_obj_clear_flag(q, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(q, LV_OBJ_FLAG_EVENT_BUBBLE);  // taps reach screen handler

    lv_obj_t *h = lv_label_create(q);
    lv_label_set_text(h, header);
    lv_obj_set_style_text_color(h, lv_color_hex(0x7faedc), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);
    return q;
}

static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05101c), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_tap_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, screen_gesture_handler, LV_EVENT_GESTURE, NULL);

    // Wind (top-left)
    lv_obj_t *q1 = make_quadrant(scr, 0, 0, "WIND");
    quadrants[0] = q1;
    lv_obj_t *ring = lv_obj_create(q1);
    lv_obj_set_size(ring, 140, 140);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(0x05101c), 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x3b6294), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 24);
    needle = lv_obj_create(q1);
    lv_obj_set_size(needle, 4, 64);
    lv_obj_set_style_bg_color(needle, lv_color_hex(0xff4d6d), 0);
    lv_obj_set_style_border_width(needle, 0, 0);
    lv_obj_set_style_radius(needle, 2, 0);
    lv_obj_align(needle, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_transform_pivot_x(needle, 2, 0);
    lv_obj_set_style_transform_pivot_y(needle, 60, 0);

    lbl_aws = lv_label_create(q1);
    lv_label_set_text(lbl_aws, "-- kn");
    lv_obj_set_style_text_color(lbl_aws, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_aws, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_aws, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lbl_awa = lv_label_create(q1);
    lv_label_set_text(lbl_awa, "---°");
    lv_obj_set_style_text_color(lbl_awa, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_awa, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_awa, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // Nav (top-right) - SOG/COG/HDG + position
    lv_obj_t *q2 = make_quadrant(scr, 1, 0, "NAV");
    quadrants[1] = q2;
    lbl_sog = lv_label_create(q2);
    lv_label_set_text(lbl_sog, "--.-");
    lv_obj_set_style_text_color(lbl_sog, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_sog, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_sog, LV_ALIGN_TOP_MID, -16, 18);

    lv_obj_t *sog_unit = lv_label_create(q2);
    lv_label_set_text(sog_unit, "kn");
    lv_obj_set_style_text_color(sog_unit, lv_color_hex(0x6c8bb1), 0);
    lv_obj_set_style_text_font(sog_unit, &lv_font_montserrat_20, 0);
    lv_obj_align(sog_unit, LV_ALIGN_TOP_MID, 60, 38);

    lbl_cog = lv_label_create(q2);
    lv_label_set_text(lbl_cog, "COG ---°");
    lv_obj_set_style_text_color(lbl_cog, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_cog, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_cog, LV_ALIGN_TOP_LEFT, 0, 88);

    lbl_hdg = lv_label_create(q2);
    lv_label_set_text(lbl_hdg, "HDG ---°");
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(0x9ec5fe), 0);
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_TOP_LEFT, 0, 122);

    lbl_pos = lv_label_create(q2);
    lv_label_set_text(lbl_pos, "---°--.---'N\n---°--.---'E");
    lv_obj_set_style_text_color(lbl_pos, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_pos, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Depth (bottom-left)
    lv_obj_t *q3 = make_quadrant(scr, 0, 1, "DEPTH / TEMP");
    quadrants[2] = q3;
    lbl_depth = lv_label_create(q3);
    lv_label_set_text(lbl_depth, "--.-");
    lv_obj_set_style_text_color(lbl_depth, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_depth, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_depth, LV_ALIGN_CENTER, -10, -10);
    lv_obj_t *dunit = lv_label_create(q3);
    lv_label_set_text(dunit, "m");
    lv_obj_set_style_text_color(dunit, lv_color_hex(0x6c8bb1), 0);
    lv_obj_set_style_text_font(dunit, &lv_font_montserrat_20, 0);
    lv_obj_align(dunit, LV_ALIGN_CENTER, 50, -10);
    lbl_temp = lv_label_create(q3);
    lv_label_set_text(lbl_temp, "water --.- °C");
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_temp, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Status (bottom-right) - device health
    lv_obj_t *q4 = make_quadrant(scr, 1, 1, "STATUS");
    quadrants[3] = q4;

    lbl_batt = lv_label_create(q4);
    lv_label_set_text(lbl_batt, "--.- V");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *batt_caption = lv_label_create(q4);
    lv_label_set_text(batt_caption, "BATTERY");
    lv_obj_set_style_text_color(batt_caption, lv_color_hex(0x6c8bb1), 0);
    lv_obj_set_style_text_font(batt_caption, &lv_font_montserrat_14, 0);
    lv_obj_align(batt_caption, LV_ALIGN_TOP_MID, 0, 64);

    lbl_status = lv_label_create(q4);
    lv_label_set_text(lbl_status, "sk: -");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, -36);

    lbl_ip = lv_label_create(q4);
    lv_label_set_text(lbl_ip, "ip ---.---.---.---");
    lv_obj_set_style_text_color(lbl_ip, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_ip, LV_ALIGN_BOTTOM_LEFT, 0, -18);

    lbl_rssi = lv_label_create(q4);
    lv_label_set_text(lbl_rssi, "rssi --- dBm");
    lv_obj_set_style_text_color(lbl_rssi, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_rssi, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_rssi, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Always-visible MOB button + rescue overlay (created after quadrants
    // so it sits on top in z-order).
    mob_build(scr);
    alarms_build(scr);
}

// --- demo mode + fps benchmark helpers ----------------------------------

static const int GRID_X[4] = {0, 1, 0, 1};
static const int GRID_Y[4] = {0, 0, 1, 1};

// focused: -1 = grid, 0..3 = center that quadrant and hide the others.
static void set_quadrant_focus(int focused) {
    for (int i = 0; i < 4; ++i) {
        if (focused < 0) {
            lv_obj_set_pos(quadrants[i], GRID_X[i] * 240 + 4, GRID_Y[i] * 240 + 4);
            lv_obj_clear_flag(quadrants[i], LV_OBJ_FLAG_HIDDEN);
        } else if (focused == i) {
            lv_obj_set_pos(quadrants[i], (LCD_W - 232) / 2, (LCD_H - 232) / 2);
            lv_obj_clear_flag(quadrants[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(quadrants[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void demo_tick(lv_timer_t *) {
    // Cycle: q0 -> q1 -> q2 -> q3 -> grid -> q0 ...
    g_demo_state = (g_demo_state + 1) % 5;
    set_quadrant_focus(g_demo_state == 4 ? -1 : g_demo_state);
}

static void demo_start(uint32_t period_ms) {
    g_demo_period_ms = period_ms ? period_ms : 3000;
    if (!g_demo_badge) {
        g_demo_badge = lv_label_create(lv_screen_active());
        lv_label_set_text(g_demo_badge, " DEMO ");
        lv_obj_set_style_bg_color(g_demo_badge, lv_color_hex(0xff4d6d), 0);
        lv_obj_set_style_bg_opa(g_demo_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(g_demo_badge, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(g_demo_badge, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(g_demo_badge, 4, 0);
        lv_obj_set_style_radius(g_demo_badge, 4, 0);
    }
    lv_obj_clear_flag(g_demo_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(g_demo_badge, LV_ALIGN_TOP_RIGHT, -6, 6);
    if (g_demo_timer) lv_timer_del(g_demo_timer);
    g_demo_state = -1;
    g_demo_timer = lv_timer_create(demo_tick, g_demo_period_ms, NULL);
    net::logf("[demo] started, %lu ms per step", (unsigned long)g_demo_period_ms);
}

static void demo_stop() {
    if (g_demo_timer) {
        lv_timer_del(g_demo_timer);
        g_demo_timer = nullptr;
    }
    if (g_demo_badge) lv_obj_add_flag(g_demo_badge, LV_OBJ_FLAG_HIDDEN);
    set_quadrant_focus(-1);
    g_focused_quad = -1;
    net::logf("[demo] stopped");
}

// ----- MOB ---------------------------------------------------------------

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
        snprintf(buf, sizeof(buf), "BRG %03.0f°", b);
        lv_label_set_text(mob_lbl_brg, buf);
        double back = b + 180.0;
        while (back >= 360)
            back -= 360;
        snprintf(buf, sizeof(buf), "return %03.0f°", back);
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
    lv_obj_set_style_bg_color(mob_button, lv_color_hex(0xff1f3a), 0);
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
    lv_obj_set_style_border_color(mob_view, lv_color_hex(0xff1f3a), 0);
    lv_obj_set_style_radius(mob_view, 0, 0);
    lv_obj_set_style_pad_all(mob_view, 16, 0);
    lv_obj_clear_flag(mob_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mob_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = lv_label_create(mob_view);
    lv_label_set_text(t, "MAN OVERBOARD");
    lv_obj_set_style_text_color(t, lv_color_hex(0xff4d6d), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    mob_lbl_dist = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_dist, "--- m");
    lv_obj_set_style_text_color(mob_lbl_dist, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mob_lbl_dist, &lv_font_montserrat_48, 0);
    lv_obj_align(mob_lbl_dist, LV_ALIGN_TOP_MID, 0, 70);

    mob_lbl_brg = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_brg, "BRG ---°");
    lv_obj_set_style_text_color(mob_lbl_brg, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mob_lbl_brg, &lv_font_montserrat_48, 0);
    lv_obj_align(mob_lbl_brg, LV_ALIGN_CENTER, 0, 10);

    mob_lbl_back = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_back, "return ---°");
    lv_obj_set_style_text_color(mob_lbl_back, lv_color_hex(0x9ec5fe), 0);
    lv_obj_set_style_text_font(mob_lbl_back, &lv_font_montserrat_20, 0);
    lv_obj_align(mob_lbl_back, LV_ALIGN_CENTER, 0, 70);

    mob_lbl_elapsed = lv_label_create(mob_view);
    lv_label_set_text(mob_lbl_elapsed, "elapsed --:--");
    lv_obj_set_style_text_color(mob_lbl_elapsed, lv_color_hex(0xeaf2ff), 0);
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

// ----- Alarms ------------------------------------------------------------

static void alarms_build(lv_obj_t *scr) {
    alarm_banner = lv_obj_create(scr);
    lv_obj_set_size(alarm_banner, 360, 36);
    lv_obj_align(alarm_banner, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(alarm_banner, lv_color_hex(0xff1f3a), 0);
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

static int hit_test_quadrant(int x, int y) {
    int q = 0;
    if (x >= LCD_W / 2) q++;
    if (y >= LCD_H / 2) q += 2;
    return q;
}

// Tap handler. Old triple-tap turned out flaky (timing-sensitive; MOB button
// blocks the top-right of q2). New gesture map:
//
//   single tap in GRID view   -> focus the tapped quadrant
//   single tap in FOCUSED     -> back to grid
//   triple-tap anywhere       -> stop demo / force back to grid
//
// Every tap is logged so users can see in BLE/UDP whether touches register.
static void screen_tap_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_mob.active) return;

    uint32_t now = millis();
    g_tap_times[0] = g_tap_times[1];
    g_tap_times[1] = g_tap_times[2];
    g_tap_times[2] = now;

    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p = {0, 0};
    if (indev) lv_indev_get_point(indev, &p);

    bool triple = (g_tap_times[0] != 0 && (now - g_tap_times[0]) < 900);
    net::logf("[ui] tap @(%d,%d) focused=%d triple=%d demo=%d", p.x, p.y, g_focused_quad,
              triple ? 1 : 0, g_demo_timer ? 1 : 0);

    if (triple) {
        g_tap_times[0] = g_tap_times[1] = g_tap_times[2] = 0;
        if (g_demo_timer) {
            demo_stop();
            return;
        }
        if (g_focused_quad >= 0) {
            set_quadrant_focus(-1);
            g_focused_quad = -1;
            return;
        }
        int q = hit_test_quadrant(p.x, p.y);
        set_quadrant_focus(q);
        g_focused_quad = q;
        return;
    }

    // Single-tap behaviour
    if (g_focused_quad >= 0) {
        set_quadrant_focus(-1);
        g_focused_quad = -1;
        return;
    }
    int q = hit_test_quadrant(p.x, p.y);
    set_quadrant_focus(q);
    g_focused_quad = q;
}

// Swipe handler. Left/right cycles adjacent quadrants; down returns to grid.
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
    net::logf("[ui] swipe %s focused=%d", dir_name, g_focused_quad);

    if (g_demo_timer) {
        demo_stop();
        return;
    }

    if (g_focused_quad < 0) {
        // From grid view: any swipe enters focus mode on a starting quadrant.
        int q = dir == LV_DIR_LEFT ? 1 : dir == LV_DIR_RIGHT ? 0 : dir == LV_DIR_TOP ? 2 : 0;
        set_quadrant_focus(q);
        g_focused_quad = q;
        return;
    }
    // In focused view: cycle adjacent quadrants, down returns to grid.
    if (dir == LV_DIR_LEFT) {
        g_focused_quad = (g_focused_quad + 1) % 4;
        set_quadrant_focus(g_focused_quad);
    } else if (dir == LV_DIR_RIGHT) {
        g_focused_quad = (g_focused_quad + 3) % 4;
        set_quadrant_focus(g_focused_quad);
    } else if (dir == LV_DIR_BOTTOM) {
        set_quadrant_focus(-1);
        g_focused_quad = -1;
    }
}

// 1 Hz FPS sampling
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
        g_fps_overlay = lv_label_create(lv_screen_active());
        lv_label_set_text(g_fps_overlay, "-- Hz");
        lv_obj_set_style_bg_color(g_fps_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(g_fps_overlay, LV_OPA_70, 0);
        lv_obj_set_style_text_color(g_fps_overlay, lv_color_hex(0x33ff99), 0);
        lv_obj_set_style_text_font(g_fps_overlay, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(g_fps_overlay, 4, 0);
        lv_obj_set_style_radius(g_fps_overlay, 4, 0);
        lv_obj_align(g_fps_overlay, LV_ALIGN_TOP_LEFT, 4, 4);
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

// Returns true if the line was consumed.
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
    if (line == "pos-format") {
        net::logf("pos-format = %s", pos_format_name(g_pos_format));
        return true;
    }
    if (line.startsWith("pos-format ")) {
        String fmt = line.substring(11);
        fmt.trim();
        PosFormat newf = g_pos_format;
        if (fmt == "ddm")
            newf = POS_DDM;
        else if (fmt == "dd")
            newf = POS_DD;
        else if (fmt == "dms")
            newf = POS_DMS;
        else {
            net::logf("usage: pos-format ddm|dd|dms  (current: %s)", pos_format_name(g_pos_format));
            return true;
        }
        g_pos_format = newf;
        Preferences p;
        p.begin("ui", false);
        p.putUChar("pos_fmt", (uint8_t)g_pos_format);
        p.end();
        net::logf("pos-format -> %s", pos_format_name(g_pos_format));
        return true;
    }
    return false;
}

static void ui_refresh(lv_timer_t *) {
    const sk::Data &d = sk::data;
    char buf[64];
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f kn", d.aws * 1.94384);
        lv_label_set_text(lbl_aws, buf);
    }
    if (!isnan(d.awa)) {
        double deg = d.awa * 180.0 / M_PI;
        if (deg < 0) deg += 360;
        snprintf(buf, sizeof(buf), "%.0f°", deg);
        lv_label_set_text(lbl_awa, buf);
        lv_obj_set_style_transform_rotation(needle, (int16_t)(deg * 10), 0);
    } else {
        // No wind data: slowly sweep the needle so the screen reads alive.
        int16_t a = (int16_t)((millis() / 4) % 3600);
        lv_obj_set_style_transform_rotation(needle, a, 0);
        lv_label_set_text(lbl_awa, "no data");
    }
    if (!isnan(d.sog)) {
        snprintf(buf, sizeof(buf), "%.1f", d.sog * 1.94384);
        lv_label_set_text(lbl_sog, buf);
    }
    if (!isnan(d.cogTrue)) {
        double deg = d.cogTrue * 180.0 / M_PI;
        if (deg < 0) deg += 360;
        snprintf(buf, sizeof(buf), "COG %.0f°", deg);
        lv_label_set_text(lbl_cog, buf);
    }
    if (!isnan(d.headingTrue)) {
        double deg = d.headingTrue * 180.0 / M_PI;
        if (deg < 0) deg += 360;
        snprintf(buf, sizeof(buf), "HDG %.0f°", deg);
        lv_label_set_text(lbl_hdg, buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        format_position(d.lat, d.lon, buf, sizeof(buf));
        lv_label_set_text(lbl_pos, buf);
    }
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        lv_label_set_text(lbl_depth, buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "water %.1f °C", d.waterTemp - 273.15);
        lv_label_set_text(lbl_temp, buf);
    }
    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "%.1f V", d.battVoltage);
        lv_label_set_text(lbl_batt, buf);
    } else {
        // No SK battery data - fall back to free heap as a "device pulse".
        snprintf(buf, sizeof(buf), "%lu kB", (unsigned long)(ESP.getFreeHeap() / 1024));
        lv_label_set_text(lbl_batt, buf);
    }
    snprintf(buf, sizeof(buf), "sk: %s", sk::connectionStatus().c_str());
    lv_label_set_text(lbl_status, buf);
    snprintf(buf, sizeof(buf), "ip %s", net::ipString().c_str());
    lv_label_set_text(lbl_ip, buf);

    // Bottom line ticks every refresh: RSSI when on STA, uptime in AP mode.
    int r = net::rssi();
    uint32_t up = millis() / 1000;
    if (r != 0) {
        snprintf(buf, sizeof(buf), "rssi %d dBm  up %lu:%02lu", r, (unsigned long)(up / 60),
                 (unsigned long)(up % 60));
    } else {
        snprintf(buf, sizeof(buf), "ap mode  up %02lu:%02lu:%02lu", (unsigned long)(up / 3600),
                 (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    }
    lv_label_set_text(lbl_rssi, buf);

    mob_refresh();
    alarm_check();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] ESP32-4848S040 hello-touch");

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);

    if (!gfx->begin()) {
        Serial.println("[gfx] begin FAILED");
        while (1)
            delay(1000);
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

    // Partial buffers in internal RAM for fast LVGL rendering.
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

    build_ui();

    digitalWrite(LCD_BL, HIGH);
    Serial.println("[boot] ready");

    net::setup();
    net::logf("[net] up - ip=%s", net::ipString().c_str());

    // Load runtime UI prefs (position format, ...) from NVS
    {
        Preferences p;
        p.begin("ui", true);
        g_pos_format = (PosFormat)p.getUChar("pos_fmt", POS_DDM);
        p.end();
        net::logf("[ui] pos-format = %s", pos_format_name(g_pos_format));
    }

    // Load layout - default baked-in for now; SignalK REST fetch later.
    layout::load_default();

    // SignalK: empty default - configure with 'sk <host> [port]' on Serial/BLE.
    sk::setup("", 3000);

    // Refresh UI labels at 5 Hz; FPS sampling at 1 Hz.
    lv_timer_create(ui_refresh, 200, NULL);
    lv_timer_create(fps_tick, 1000, NULL);

    // Route demo / fps / bench commands through main when net+sk don't claim them.
    net::setExtraCommandHandler(handleMainCommand);
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
    pollSerialCommands();
    delay(5);
}
