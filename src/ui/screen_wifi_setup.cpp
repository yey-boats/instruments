#include "screens.h"
#include "ui_theme.h"
#include "net.h"
#include "board_pins.h"

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Fullscreen WiFi setup. Two views in one screen, switched via internal
// state:
//   LIST  - SCAN button + list of SSIDs (most recent scan)
//   ENTRY - selected SSID + password text-area + on-screen keyboard
// Save triggers `wifi <ssid> <pass>` through net::dispatchCommand, which
// persists to NVS and reboots.

namespace ui::wifi_setup {

enum class View { Provision, List, Entry };
static View s_view = View::Provision;

static lv_obj_t *s_root = nullptr;
static lv_obj_t *provision_view = nullptr;  // AP-mode QR provisioning
static lv_obj_t *list_view = nullptr;
static lv_obj_t *entry_view = nullptr;
static lv_obj_t *ssid_list = nullptr;  // scrolling container of buttons
static lv_obj_t *btn_scan = nullptr;
static lv_obj_t *lbl_scan_status = nullptr;

static lv_obj_t *qr_code = nullptr;
static lv_obj_t *lbl_ap_url = nullptr;

static lv_obj_t *lbl_selected_ssid = nullptr;
static lv_obj_t *ta_pass = nullptr;
static lv_obj_t *kb = nullptr;
static lv_obj_t *btn_connect = nullptr;
static lv_obj_t *btn_back = nullptr;

static String s_selected_ssid;
static bool s_scanning = false;

static void show_provision() {
    s_view = View::Provision;
    if (provision_view) lv_obj_clear_flag(provision_view, LV_OBJ_FLAG_HIDDEN);
    if (list_view) lv_obj_add_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    if (entry_view) lv_obj_add_flag(entry_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_list() {
    s_view = View::List;
    if (provision_view) lv_obj_add_flag(provision_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(entry_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_entry(const String &ssid) {
    s_view = View::Entry;
    s_selected_ssid = ssid;
    if (lbl_selected_ssid) lv_label_set_text(lbl_selected_ssid, ssid.c_str());
    if (ta_pass) lv_textarea_set_text(ta_pass, "");
    if (provision_view) lv_obj_add_flag(provision_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(entry_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_ssid_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid) return;
    show_entry(String(ssid));
}

static void populate_list_from_scan(int found) {
    lv_obj_clean(ssid_list);
    if (found <= 0) {
        lv_obj_t *empty = lv_label_create(ssid_list);
        lv_label_set_text(empty, "no networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
        return;
    }
    for (int i = 0; i < found && i < 20; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        int rssi = WiFi.RSSI(i);
        wifi_auth_mode_t auth = WiFi.encryptionType(i);
        bool secured = (auth != WIFI_AUTH_OPEN);

        lv_obj_t *b = lv_obj_create(ssid_list);
        lv_obj_set_size(b, lv_pct(100), 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(theme.panel_edge), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_pad_all(b, 4, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        // Allocate persistent storage for the ssid string (the WiFi.SSID copy
        // would go out of scope). Leak-cleanup happens on lv_obj_clean.
        char *ssid_buf = (char *)lv_malloc(ssid.length() + 1);
        memcpy(ssid_buf, ssid.c_str(), ssid.length() + 1);
        lv_obj_add_event_cb(b, on_ssid_clicked, LV_EVENT_CLICKED, ssid_buf);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, ssid.c_str());
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(theme.fg), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        char meta[32];
        snprintf(meta, sizeof(meta), "%d dBm %s", rssi, secured ? "lock" : "open");
        lv_obj_t *m = lv_label_create(b);
        lv_label_set_text(m, meta);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void on_scan_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_scanning) return;
    s_scanning = true;
    lv_label_set_text(lbl_scan_status, "scanning...");
    // Async kick - returns immediately. We poll WiFi.scanComplete() from
    // refresh() so the UI stays responsive while the scan runs (~2-5 s).
    WiFi.scanNetworks(true /* async */, true /* show hidden */);
}

static void on_connect_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected_ssid.length() == 0) return;
    const char *pass = lv_textarea_get_text(ta_pass);
    String cmd = String("wifi ") + s_selected_ssid + " " + pass;
    net::logf("[wifi-setup] save + reboot");
    net::handleSerialCommand(cmd);
}

static void on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_list();
}

static void on_ta_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) return;
    if (kb) lv_keyboard_set_textarea(kb, ta_pass);
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

    // ---- PROVISION VIEW (shown when device is in AP mode) ----
    provision_view = lv_obj_create(s_root);
    lv_obj_set_size(provision_view, LCD_W, LCD_H);
    lv_obj_set_pos(provision_view, 0, 0);
    lv_obj_set_style_bg_color(provision_view, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(provision_view, 0, 0);
    lv_obj_set_style_pad_all(provision_view, 16, 0);
    lv_obj_clear_flag(provision_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(provision_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *prov_title = lv_label_create(provision_view);
    lv_label_set_text(prov_title, "JOIN TO CONFIGURE");
    lv_obj_set_style_text_font(prov_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(prov_title, lv_color_hex(theme.accent), 0);
    lv_obj_align(prov_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *prov_sub = lv_label_create(provision_view);
    lv_label_set_text(prov_sub, "1. scan QR or join \"espdisp-setup\"\n"
                                "2. open http://192.168.4.1/\n"
                                "3. pick a network in the WIFI panel");
    lv_obj_set_style_text_font(prov_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prov_sub, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(prov_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prov_sub, LV_ALIGN_TOP_MID, 0, 28);

    // QR encodes the WiFi-join URI; phones scan -> auto-join the open AP.
    // Pixel buffer (~115 kB at 240x240x2bpp) comes from PSRAM via the
    // custom LVGL allocators in src/lvgl_alloc.cpp.
    qr_code = lv_qrcode_create(provision_view);
    lv_qrcode_set_size(qr_code, 220);
    lv_qrcode_set_dark_color(qr_code, lv_color_hex(0x0a1a2b));
    lv_qrcode_set_light_color(qr_code, lv_color_hex(0xffffff));
    const char *wifi_uri = "WIFI:T:nopass;S:espdisp-setup;;";
    lv_qrcode_update(qr_code, wifi_uri, strlen(wifi_uri));
    lv_obj_align(qr_code, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_border_width(qr_code, 6, 0);
    lv_obj_set_style_border_color(qr_code, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_radius(qr_code, 6, 0);

    lbl_ap_url = lv_label_create(provision_view);
    lv_label_set_text(lbl_ap_url, "espdisp-setup  ->  http://192.168.4.1/");
    lv_obj_set_style_text_font(lbl_ap_url, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ap_url, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_ap_url, LV_ALIGN_BOTTOM_MID, 0, -8);

    // ---- LIST VIEW ----
    list_view = lv_obj_create(s_root);
    lv_obj_set_size(list_view, LCD_W, LCD_H);
    lv_obj_set_pos(list_view, 0, 0);
    lv_obj_set_style_bg_color(list_view, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(list_view, 0, 0);
    lv_obj_set_style_pad_all(list_view, 8, 0);
    lv_obj_clear_flag(list_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_view, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *title = lv_label_create(list_view);
    lv_label_set_text(title, "WIFI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    btn_scan = lv_button_create(list_view);
    lv_obj_set_size(btn_scan, 120, 44);
    // Top-right reserved for global MOB pill - drop scan below it.
    lv_obj_align(btn_scan, LV_ALIGN_TOP_RIGHT, -8, 72);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_radius(btn_scan, 8, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(btn_scan);
    lv_label_set_text(scan_lbl, "SCAN");
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(scan_lbl);

    lbl_scan_status = lv_label_create(list_view);
    lv_label_set_text(lbl_scan_status, "tap SCAN to search");
    lv_obj_set_style_text_font(lbl_scan_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_scan_status, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_scan_status, LV_ALIGN_TOP_LEFT, 8, 56);

    ssid_list = lv_obj_create(list_view);
    lv_obj_set_size(ssid_list, LCD_W - 16, LCD_H - 90);
    lv_obj_align(ssid_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(ssid_list, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_color(ssid_list, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(ssid_list, 1, 0);
    lv_obj_set_style_radius(ssid_list, 6, 0);
    lv_obj_set_style_pad_all(ssid_list, 6, 0);
    lv_obj_set_flex_flow(ssid_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(ssid_list, 6, 0);

    // ---- ENTRY VIEW ----
    entry_view = lv_obj_create(s_root);
    lv_obj_set_size(entry_view, LCD_W, LCD_H);
    lv_obj_set_pos(entry_view, 0, 0);
    lv_obj_set_style_bg_color(entry_view, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(entry_view, 0, 0);
    lv_obj_set_style_pad_all(entry_view, 6, 0);
    lv_obj_clear_flag(entry_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(entry_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *entry_title = lv_label_create(entry_view);
    lv_label_set_text(entry_title, "PASSWORD");
    lv_obj_set_style_text_font(entry_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(entry_title, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(entry_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_selected_ssid = lv_label_create(entry_view);
    lv_label_set_text(lbl_selected_ssid, "");
    lv_obj_set_style_text_font(lbl_selected_ssid, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_selected_ssid, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_selected_ssid, LV_ALIGN_TOP_LEFT, 0, 16);

    ta_pass = lv_textarea_create(entry_view);
    lv_obj_set_size(ta_pass, LCD_W - 12, 44);
    lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, false);
    lv_textarea_set_placeholder_text(ta_pass, "password");
    lv_obj_add_event_cb(ta_pass, on_ta_clicked, LV_EVENT_FOCUSED, NULL);

    btn_back = lv_button_create(entry_view);
    lv_obj_set_size(btn_back, 90, 38);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -100, 92);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, "back");
    lv_obj_center(back_lbl);

    btn_connect = lv_button_create(entry_view);
    lv_obj_set_size(btn_connect, 90, 38);
    lv_obj_align(btn_connect, LV_ALIGN_TOP_RIGHT, -4, 92);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(theme.good), 0);
    lv_obj_set_style_radius(btn_connect, 8, 0);
    lv_obj_add_event_cb(btn_connect, on_connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *con_lbl = lv_label_create(btn_connect);
    lv_label_set_text(con_lbl, "connect");
    lv_obj_center(con_lbl);

    kb = lv_keyboard_create(entry_view);
    lv_obj_set_size(kb, LCD_W - 12, 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_keyboard_set_textarea(kb, ta_pass);

    // "scan on-screen instead" button on the provision view
    lv_obj_t *btn_list = lv_button_create(provision_view);
    lv_obj_set_size(btn_list, 200, 36);
    lv_obj_align(btn_list, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_style_bg_color(btn_list, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_radius(btn_list, 8, 0);
    lv_obj_add_event_cb(
        btn_list,
        [](lv_event_t *e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_list();
        },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *list_btn_lbl = lv_label_create(btn_list);
    lv_label_set_text(list_btn_lbl, "scan on-screen instead");
    lv_obj_center(list_btn_lbl);

    // Initial view depends on whether we have WiFi up. AP mode = provision.
    if (!net::wifiUp()) {
        show_provision();
    } else {
        show_list();
    }

    return s_root;
}

void refresh() {
    // Poll the async scan: -1 = running, -2 = no scan, >=0 = N results.
    if (!s_scanning) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        // Update spinner-style status so the user sees activity.
        static uint8_t dots = 0;
        char status[24];
        snprintf(status, sizeof(status), "scanning%s",
                 (dots % 4) == 0   ? ""
                 : (dots % 4) == 1 ? "."
                 : (dots % 4) == 2 ? ".."
                                   : "...");
        lv_label_set_text(lbl_scan_status, status);
        dots++;
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        lv_label_set_text(lbl_scan_status, "scan failed");
        s_scanning = false;
        return;
    }
    // Done - render results
    populate_list_from_scan(n);
    char status[32];
    snprintf(status, sizeof(status), "%d network%s", n, n == 1 ? "" : "s");
    lv_label_set_text(lbl_scan_status, status);
    WiFi.scanDelete();
    s_scanning = false;
}

}  // namespace ui::wifi_setup
