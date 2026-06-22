#include "screens.h"
#include "ui_theme.h"
#include "ui_dirty.h"
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
    style_caption(l);
    lv_obj_set_pos(l, 12, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "-");
    style_value(v, &lv_font_montserrat_20, theme.fg);
    lv_obj_set_pos(v, 120, y - 4);
    return v;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int y, uint32_t color) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, 180, 8);
    lv_obj_set_pos(b, 270, y + 6);
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
    style_screen(s_root);

    // Title
    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "SYSTEM");
    style_value(title, &lv_font_montserrat_28, theme.accent);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *panel = lv_obj_create(s_root);
    lv_obj_set_size(panel, LCD_W - 16, 412);
    lv_obj_set_pos(panel, 8, 58);
    style_panel(panel, theme.accent);
    panel_accent(panel, theme.accent);

    // Rows
    int y = 10;
    const int ROW_H = 28;
    r_batt.value = make_row(panel, "BATTERY", y);
    r_batt.bar = nullptr;
    y += ROW_H;
    r_soc.value = make_row(panel, "SoC", y);
    r_soc.bar = make_bar(panel, y, theme.good);
    y += ROW_H;
    r_fuel.value = make_row(panel, "FUEL", y);
    r_fuel.bar = make_bar(panel, y, theme.warn);
    y += ROW_H;
    r_water.value = make_row(panel, "WATER", y);
    r_water.bar = make_bar(panel, y, theme.accent);
    y += ROW_H;
    r_wifi_mode = make_row(panel, "WIFI", y);
    y += ROW_H;
    r_ssid = make_row(panel, "SSID", y);
    y += ROW_H;
    r_ip = make_row(panel, "IP", y);
    y += ROW_H;
    r_rssi = make_row(panel, "RSSI", y);
    y += ROW_H;
    r_ble = make_row(panel, "BLE", y);
    y += ROW_H;
    r_sk = make_row(panel, "SIGNALK", y);
    y += ROW_H;
    r_heap = make_row(panel, "HEAP", y);
    y += ROW_H;
    r_psram = make_row(panel, "PSRAM", y);
    y += ROW_H;
    r_uptime = make_row(panel, "UPTIME", y);
    y += ROW_H;
    r_fw = make_row(panel, "BUILD", y);

    return s_root;
}

// Dirty-value caches (docs/specs/09).
static char s_last_batt[16] = {(char)0xFF};
static char s_last_soc[16] = {(char)0xFF};
static char s_last_fuel[16] = {(char)0xFF};
static char s_last_water[16] = {(char)0xFF};
static char s_last_wifi_mode[8] = {(char)0xFF};
static char s_last_ssid[40] = {(char)0xFF};
static char s_last_ip[24] = {(char)0xFF};
static char s_last_rssi[16] = {(char)0xFF};
static char s_last_ble[32] = {(char)0xFF};
static char s_last_sk[24] = {(char)0xFF};
static char s_last_heap[16] = {(char)0xFF};
static char s_last_psram[16] = {(char)0xFF};
static char s_last_uptime[16] = {(char)0xFF};
static char s_last_fw[24] = {(char)0xFF};
static int s_last_soc_bar = -1;
static int s_last_fuel_bar = -1;
static int s_last_water_bar = -1;

void refresh() {
    boat::View d_snap;
    boat::current_view(d_snap);
    const boat::View &d = d_snap;
    char buf[64];

    if (!isnan(d.battVoltage)) {
        snprintf(buf, sizeof(buf), "%.2f V", d.battVoltage);
        set_text_if_changed(r_batt.value, s_last_batt, sizeof(s_last_batt), buf);
    } else {
        set_text_if_changed(r_batt.value, s_last_batt, sizeof(s_last_batt), "-");
    }

    if (!isnan(d.battSoc)) {
        int pct = (int)(d.battSoc * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        set_text_if_changed(r_soc.value, s_last_soc, sizeof(s_last_soc), buf);
        if (r_soc.bar && pct != s_last_soc_bar) {
            s_last_soc_bar = pct;
            lv_bar_set_value(r_soc.bar, pct, LV_ANIM_OFF);
        }
    } else {
        set_text_if_changed(r_soc.value, s_last_soc, sizeof(s_last_soc), "-");
    }

    if (!isnan(d.tankFuel)) {
        int pct = (int)(d.tankFuel * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        set_text_if_changed(r_fuel.value, s_last_fuel, sizeof(s_last_fuel), buf);
        if (r_fuel.bar && pct != s_last_fuel_bar) {
            s_last_fuel_bar = pct;
            lv_bar_set_value(r_fuel.bar, pct, LV_ANIM_OFF);
        }
    } else {
        set_text_if_changed(r_fuel.value, s_last_fuel, sizeof(s_last_fuel), "-");
    }

    if (!isnan(d.tankWater)) {
        int pct = (int)(d.tankWater * 100.0);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        set_text_if_changed(r_water.value, s_last_water, sizeof(s_last_water), buf);
        if (r_water.bar && pct != s_last_water_bar) {
            s_last_water_bar = pct;
            lv_bar_set_value(r_water.bar, pct, LV_ANIM_OFF);
        }
    } else {
        set_text_if_changed(r_water.value, s_last_water, sizeof(s_last_water), "-");
    }

    bool sta = net::wifiUp();
    set_text_if_changed(r_wifi_mode, s_last_wifi_mode, sizeof(s_last_wifi_mode),
                        sta ? "STA" : "AP");
    String ssid = sta ? WiFi.SSID() : "yey-d-setup";
    set_text_if_changed(r_ssid, s_last_ssid, sizeof(s_last_ssid),
                        ssid.length() ? ssid.c_str() : "-");
    set_text_if_changed(r_ip, s_last_ip, sizeof(s_last_ip), net::ipString().c_str());

    int r = net::rssi();
    if (sta)
        snprintf(buf, sizeof(buf), "%d dBm", r);
    else
        snprintf(buf, sizeof(buf), "n/a");
    set_text_if_changed(r_rssi, s_last_rssi, sizeof(s_last_rssi), buf);

    set_text_if_changed(r_ble, s_last_ble, sizeof(s_last_ble), net::deviceId().c_str());
    set_text_if_changed(r_sk, s_last_sk, sizeof(s_last_sk), sk::connectionStatus().c_str());

    snprintf(buf, sizeof(buf), "%lu kB", (unsigned long)(ESP.getFreeHeap() / 1024));
    set_text_if_changed(r_heap, s_last_heap, sizeof(s_last_heap), buf);
    snprintf(buf, sizeof(buf), "%lu kB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    set_text_if_changed(r_psram, s_last_psram, sizeof(s_last_psram), buf);

    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)(up / 3600),
             (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    set_text_if_changed(r_uptime, s_last_uptime, sizeof(s_last_uptime), buf);

    set_text_if_changed(r_fw, s_last_fw, sizeof(s_last_fw), __DATE__);
}

}  // namespace ui::status_panel
