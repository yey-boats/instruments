#include "screens.h"
#include "ui_theme.h"
#include "signalk.h"
#include "net.h"
#include "board_pins.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Esp.h>
#include <math.h>
#include <stdio.h>

// Fullscreen system / device health page. Two columns of (label : value),
// with bars for battery SoC and tanks where SignalK provides them.

namespace ui::status_panel {

struct Row {
    lv_obj_t *value;
    lv_obj_t *bar;  // optional, may be nullptr
};

static lv_obj_t *s_root = nullptr;
static Row r_batt, r_soc, r_fuel, r_water;
static lv_obj_t *r_wifi_mode, *r_ssid, *r_ip, *r_rssi;
static lv_obj_t *r_ble, *r_sk, *r_heap, *r_psram, *r_uptime, *r_fw;

static lv_obj_t *make_row(lv_obj_t *parent, const char *label, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(l, 12, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(theme.fg), 0);
    lv_obj_set_pos(v, 120, y - 4);
    return v;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int y, uint32_t color) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, 200, 8);
    lv_obj_set_pos(b, 260, y + 6);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel_edge), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 4, 0);
    return b;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Title
    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "SYSTEM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Rows
    int y = 56;
    const int ROW_H = 32;
    r_batt.value = make_row(s_root, "BATTERY", y);
    r_batt.bar = nullptr;
    y += ROW_H;
    r_soc.value = make_row(s_root, "SoC", y);
    r_soc.bar = make_bar(s_root, y, theme.good);
    y += ROW_H;
    r_fuel.value = make_row(s_root, "FUEL", y);
    r_fuel.bar = make_bar(s_root, y, theme.warn);
    y += ROW_H;
    r_water.value = make_row(s_root, "WATER", y);
    r_water.bar = make_bar(s_root, y, theme.accent);
    y += ROW_H;
    r_wifi_mode = make_row(s_root, "WIFI", y);
    y += ROW_H;
    r_ssid = make_row(s_root, "SSID", y);
    y += ROW_H;
    r_ip = make_row(s_root, "IP", y);
    y += ROW_H;
    r_rssi = make_row(s_root, "RSSI", y);
    y += ROW_H;
    r_ble = make_row(s_root, "BLE", y);
    y += ROW_H;
    r_sk = make_row(s_root, "SIGNALK", y);
    y += ROW_H;
    r_heap = make_row(s_root, "HEAP", y);
    y += ROW_H;
    r_psram = make_row(s_root, "PSRAM", y);
    y += ROW_H;
    r_uptime = make_row(s_root, "UPTIME", y);
    y += ROW_H;
    r_fw = make_row(s_root, "BUILD", y);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "%.2f V", d.battVoltage);
        lv_label_set_text(r_batt.value, buf);
    } else
        lv_label_set_text(r_batt.value, "-");

    if (!isnan(d.battSoc)) {
        int pct = (int)(d.battSoc * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(r_soc.value, buf);
        if (r_soc.bar) lv_bar_set_value(r_soc.bar, pct, LV_ANIM_OFF);
    } else
        lv_label_set_text(r_soc.value, "-");

    if (!isnan(d.tankFuel)) {
        int pct = (int)(d.tankFuel * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(r_fuel.value, buf);
        if (r_fuel.bar) lv_bar_set_value(r_fuel.bar, pct, LV_ANIM_OFF);
    } else
        lv_label_set_text(r_fuel.value, "-");

    if (!isnan(d.tankWater)) {
        int pct = (int)(d.tankWater * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(r_water.value, buf);
        if (r_water.bar) lv_bar_set_value(r_water.bar, pct, LV_ANIM_OFF);
    } else
        lv_label_set_text(r_water.value, "-");

    bool sta = net::wifiUp();
    lv_label_set_text(r_wifi_mode, sta ? "STA" : "AP");
    String ssid = sta ? WiFi.SSID() : "espdisp-setup";
    lv_label_set_text(r_ssid, ssid.length() ? ssid.c_str() : "-");
    lv_label_set_text(r_ip, net::ipString().c_str());

    int r = net::rssi();
    if (sta) snprintf(buf, sizeof(buf), "%d dBm", r);
    else snprintf(buf, sizeof(buf), "n/a");
    lv_label_set_text(r_rssi, buf);

    lv_label_set_text(r_ble, net::deviceId().c_str());
    lv_label_set_text(r_sk, sk::connectionStatus().c_str());

    snprintf(buf, sizeof(buf), "%lu kB", (unsigned long)(ESP.getFreeHeap() / 1024));
    lv_label_set_text(r_heap, buf);
    snprintf(buf, sizeof(buf), "%lu kB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(r_psram, buf);

    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)(up / 3600),
             (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    lv_label_set_text(r_uptime, buf);

    lv_label_set_text(r_fw, __DATE__);
}

}  // namespace ui::status_panel
