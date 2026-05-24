#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
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

static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(d);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
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
            uint16_t x = Wire.read() | (Wire.read() << 8);
            uint16_t y = Wire.read() | (Wire.read() << 8);
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
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
static lv_obj_t *lbl_sog, *lbl_cog;
static lv_obj_t *lbl_depth, *lbl_temp;
static lv_obj_t *lbl_pos, *lbl_batt, *lbl_status;

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

    // Wind (top-left)
    lv_obj_t *q1 = make_quadrant(scr, 0, 0, "WIND  AWA / AWS");
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

    // Nav (top-right)
    lv_obj_t *q2 = make_quadrant(scr, 1, 0, "NAV  SOG / COG");
    lbl_sog = lv_label_create(q2);
    lv_label_set_text(lbl_sog, "--.-");
    lv_obj_set_style_text_color(lbl_sog, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_sog, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_sog, LV_ALIGN_CENTER, -20, -10);
    lv_obj_t *unit = lv_label_create(q2);
    lv_label_set_text(unit, "kn");
    lv_obj_set_style_text_color(unit, lv_color_hex(0x6c8bb1), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
    lv_obj_align(unit, LV_ALIGN_CENTER, 60, -10);
    lbl_cog = lv_label_create(q2);
    lv_label_set_text(lbl_cog, "COG ---°");
    lv_obj_set_style_text_color(lbl_cog, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_cog, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_cog, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Depth (bottom-left)
    lv_obj_t *q3 = make_quadrant(scr, 0, 1, "DEPTH / TEMP");
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

    // Status (bottom-right)
    lv_obj_t *q4 = make_quadrant(scr, 1, 1, "STATUS");
    lbl_pos = lv_label_create(q4);
    lv_label_set_text(lbl_pos, "pos ---.---");
    lv_obj_set_style_text_color(lbl_pos, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_pos, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_pos, LV_ALIGN_TOP_LEFT, 0, 26);
    lbl_batt = lv_label_create(q4);
    lv_label_set_text(lbl_batt, "batt --.- V");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_CENTER, 0, 0);
    lbl_status = lv_label_create(q4);
    lv_label_set_text(lbl_status, "sk: -");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x6c8bb1), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
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
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        lv_label_set_text(lbl_depth, buf);
    }
    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "water %.1f °C", d.waterTemp - 273.15);
        lv_label_set_text(lbl_temp, buf);
    }
    if (!isnan(d.lat) && !isnan(d.lon)) {
        snprintf(buf, sizeof(buf), "%+.4f\n%+.4f", d.lat, d.lon);
        lv_label_set_text(lbl_pos, buf);
    }
    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "batt %.1f V", d.battVoltage);
        lv_label_set_text(lbl_batt, buf);
    }
    snprintf(buf, sizeof(buf), "sk: %s", sk::connectionStatus().c_str());
    lv_label_set_text(lbl_status, buf);
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

    // SignalK: default to laptop's hotspot IP. Override with: sk <host> [port]
    sk::setup("10.179.64.84", 3000);

    // Refresh UI labels at 5 Hz
    lv_timer_create(ui_refresh, 200, NULL);
}

static String serial_line;

static void pollSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            serial_line.trim();
            if (serial_line.length()) {
                if (!net::handleSerialCommand(serial_line)) {
                    sk::handleSerialCommand(serial_line);
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
