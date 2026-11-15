#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_screens.h"
#include "app_events.h"
#include "net.h"
#include "theme_choice.h"
#include "trip_log.h"
#include "manager.h"
#include "web_auth_policy.h"
#include "web_login_info.h"
#include "web_pw_store.h"

#include "board_pins.h"
#include "build_config.h"

#include <Arduino.h>
#include "storage.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

// Settings screen - hidden from the swipe cycle, opened by swipe-up from any
// screen. Uses single segmented controls for choices so state is visible and
// the touch target is one coherent control per setting.

namespace ui::settings {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *brightness_value = nullptr;
static lv_obj_t *depth_value = nullptr;
static lv_obj_t *battery_value = nullptr;
static lv_obj_t *web_who = nullptr;
static lv_obj_t *web_where = nullptr;
static uint32_t web_refreshed_ms = 0;

static constexpr uint8_t BRIGHTNESS_VALUES[] = {32, 80, 128, 200, 255};
static constexpr double DEPTH_VALUES[] = {1.0, 2.0, 3.0, 5.0, 10.0};
static constexpr double BATTERY_VALUES[] = {10.5, 11.0, 11.5, 12.0, 12.2};

struct Segmented {
    lv_obj_t *buttons[5] = {};
    uint8_t count = 0;
    int selected = -1;
};

static Segmented seg_brightness;
static Segmented seg_theme;
static Segmented seg_format;
#if YEYBOATS_ENABLE_DEMO
static Segmented seg_demo;
#endif
static Segmented seg_depth;
static Segmented seg_battery;
#if YEYBOATS_ENABLE_DEMO
static bool demo_enabled = false;
#endif

static int nearest_brightness_index(uint8_t value) {
    int best = 0;
    int best_delta = 999;
    for (uint8_t i = 0; i < sizeof(BRIGHTNESS_VALUES); ++i) {
        int delta = abs((int)value - (int)BRIGHTNESS_VALUES[i]);
        if (delta < best_delta) {
            best = i;
            best_delta = delta;
        }
    }
    return best;
}

static int nearest_double_index(double value, const double *values, uint8_t count) {
    int best = 0;
    double best_delta = 1e9;
    for (uint8_t i = 0; i < count; ++i) {
        double delta = fabs(value - values[i]);
        if (delta < best_delta) {
            best = i;
            best_delta = delta;
        }
    }
    return best;
}

static void update_segment(Segmented &seg, int selected) {
    seg.selected = selected;
    for (uint8_t i = 0; i < seg.count; ++i) {
        bool active = (i == selected);
        // Button tokens (#67), tested for AA on every skin.
        uint32_t bg = active ? theme.button_active_bg : theme.button_bg;
        uint32_t fg = active ? theme.button_active_fg : theme.button_fg;
        lv_obj_set_style_bg_color(seg.buttons[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(seg.buttons[i], lv_color_hex(bg), 0);
        lv_obj_t *label = lv_obj_get_child(seg.buttons[i], 0);
        if (label) lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    style_caption(label);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *make_value_label(lv_obj_t *parent, int x, int y, int w) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    style_value(label, &font_b612_14, theme.fg_dim);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void make_segmented(lv_obj_t *parent, Segmented &seg, const char *const *labels,
                           uint8_t count, int x, int y, int w, int h, lv_event_cb_t cb) {
    seg.count = count;
    int item_w = w / count;
    for (uint8_t i = 0; i < count; ++i) {
        lv_obj_t *button = lv_button_create(parent);
        seg.buttons[i] = button;
        lv_obj_set_size(button, i == count - 1 ? w - item_w * i : item_w, h);
        lv_obj_set_pos(button, x + item_w * i, y);
        lv_obj_set_style_radius(button, 0, 0);
        if (i == 0) {
            lv_obj_set_style_radius(button, 8, 0);
            lv_obj_set_style_border_side(button, LV_BORDER_SIDE_FULL, 0);
        } else if (i == count - 1) {
            lv_obj_set_style_radius(button, 8, 0);
            lv_obj_set_style_border_side(button, LV_BORDER_SIDE_FULL, 0);
        }
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &font_b612_14, 0);
        lv_obj_center(label);
    }
}

static lv_obj_t *make_action(lv_obj_t *parent, const char *text, int x, int y, int w,
                             uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, w, 42);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(theme.bg), 0);  // ink on accent fill
    lv_obj_set_style_text_font(label, &font_b612_14, 0);
    lv_obj_center(label);
    return button;
}

static String current_theme_pref() {
    storage::Namespace p("ui", true);
    return String(p.get_string("theme", "night").c_str());
}

static void update_brightness_label() {
    if (!brightness_value) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)((brightness() * 100 + 127) / 255));
    lv_label_set_text(brightness_value, buf);
}

static void update_depth_label() {
    if (!depth_value) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f m", depth_alarm_m());
    lv_label_set_text(depth_value, buf);
}

static void update_battery_label() {
    if (!battery_value) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f V", battery_alarm_v());
    lv_label_set_text(battery_value, buf);
}

static void on_brightness(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    set_brightness(BRIGHTNESS_VALUES[index]);
    update_segment(seg_brightness, index);
    update_brightness_label();
}

static void on_theme(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const char *name = theme_choice::theme_for(index);
    if (!name) return;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "theme %s", name);
    net::dispatchCommand(cmd);
    update_segment(seg_theme, index);
}

static void on_format(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index == 0) set_pos_format(PosFormat::DDM);
    if (index == 1) set_pos_format(PosFormat::DD);
    if (index == 2) set_pos_format(PosFormat::DMS);
    update_segment(seg_format, index);
}

static void on_demo(lv_event_t *e) {
#if YEYBOATS_ENABLE_DEMO
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    demo_enabled = (index == 1);
    net::dispatchCommand(demo_enabled ? "demo 4" : "demo-off");
    update_segment(seg_demo, index);
#else
    (void)e;
#endif
}

static void on_depth(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    set_depth_alarm_m(DEPTH_VALUES[index]);
    update_segment(seg_depth, index);
    update_depth_label();
}

static void on_battery(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    set_battery_alarm_v(BATTERY_VALUES[index]);
    update_segment(seg_battery, index);
    update_battery_label();
}

static void on_trip_reset(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    trip_log::reset();
}

static void on_open_wifi(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::show_by_id("wifi");
}

static void on_open_touch_cal(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::show_by_id("touch_cal");
}

// The "web login" row (web_login_info.h): username, address + /editor and
// where the password is. The device keeps only a hash of the password
// (instruments#30), so it is never shown here. UI task only; reads NVS, so
// refresh() rate-limits it.
static void update_web_login() {
    if (!web_who || !web_where) return;
    std::string user;
    uint8_t flag;
    {
        storage::Namespace p("web", true);
        user = p.get_string("user", web_auth_policy::DEFAULT_USER);
        flag = p.get_u8("auth", web_auth_policy::FLAG_DEFAULT);
    }
    String ip = net::ipString();
    web_login_info::Input in{web_auth_policy::state(flag, web_pw_store::is_set() ? "set" : ""),
                             user.c_str(), ip.c_str(), manager::is_provisioned()};
    char who[80], where[64];
    web_login_info::describe(in, who, sizeof(who), where, sizeof(where));
    lv_label_set_text(web_who, who);
    lv_label_set_text(web_where, where);
    web_refreshed_ms = millis();
}

static void on_root_delete(lv_event_t *) {
    web_who = nullptr;
    web_where = nullptr;
}

static void on_close(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    // "home" = the index defaultScreen (MIDL records); the funnel returns to
    // the screen settings was opened from when there is one.
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, "home", sizeof(c.a) - 1);
    app::post(c, 0);
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(s_root, 0, 0);
    style_screen(s_root);
    lv_obj_set_style_pad_all(s_root, 8, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_root, on_root_delete, LV_EVENT_DELETE, nullptr);

    make_action(s_root, "close", 12, 8, 80, theme.fg_dim, on_close);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "SETTINGS");
    style_value(title, &font_b612_28, theme.accent);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 112, 12);

    static const char *const brightness_labels[] = {"20", "35", "50", "80", "100"};
    static const char *const theme_labels[] = {"DAY", "NIGHT"};
    static const char *const format_labels[] = {"DDM", "DD", "DMS"};
#if YEYBOATS_ENABLE_DEMO
    static const char *const demo_labels[] = {"OFF", "ON"};
#endif
    static const char *const depth_labels[] = {"1", "2", "3", "5", "10"};
    static const char *const battery_labels[] = {"10.5", "11.0", "11.5", "12.0", "12.2"};

    const int label_x = 16;
    const int value_x = 88;
    const int segment_x = 150;
    const int segment_w = 318;
    // 48 (was 54) leaves room for the web login row at the bottom.
    const int row_h = 48;
    // y0 must clear the global MOB pill (LV_ALIGN_TOP_RIGHT -6, 6,
    // 56x56 = y reaches 62) so the topmost segmented control's right
    // edge can't be intercepted by MOB.
    const int y0 = 72;

    make_label(s_root, "BRIGHT", label_x, y0 + 12);
    brightness_value = make_value_label(s_root, value_x, y0 + 12, 52);
    make_segmented(s_root, seg_brightness, brightness_labels, 5, segment_x, y0, segment_w, 40,
                   on_brightness);

    make_label(s_root, "THEME", label_x, y0 + row_h + 12);
    make_segmented(s_root, seg_theme, theme_labels, 2, segment_x, y0 + row_h, segment_w, 40,
                   on_theme);

    make_label(s_root, "FORMAT", label_x, y0 + row_h * 2 + 12);
    make_segmented(s_root, seg_format, format_labels, 3, segment_x, y0 + row_h * 2, segment_w, 40,
                   on_format);

    make_label(s_root, "DEPTH", label_x, y0 + row_h * 3 + 12);
    depth_value = make_value_label(s_root, value_x, y0 + row_h * 3 + 12, 52);
    make_segmented(s_root, seg_depth, depth_labels, 5, segment_x, y0 + row_h * 3, segment_w, 40,
                   on_depth);

    make_label(s_root, "BATTERY", label_x, y0 + row_h * 4 + 12);
    battery_value = make_value_label(s_root, value_x, y0 + row_h * 4 + 12, 52);
    make_segmented(s_root, seg_battery, battery_labels, 5, segment_x, y0 + row_h * 4, segment_w, 40,
                   on_battery);

#if YEYBOATS_ENABLE_DEMO
    make_label(s_root, "DEMO", label_x, y0 + row_h * 5 + 12);
    make_segmented(s_root, seg_demo, demo_labels, 2, segment_x, y0 + row_h * 5, 154, 40, on_demo);
    make_action(s_root, "trip reset", 318, y0 + row_h * 5 - 1, 150, theme.warn, on_trip_reset);
#else
    make_action(s_root, "trip reset", segment_x, y0 + row_h * 5 - 1, 154, theme.warn,
                on_trip_reset);
#endif

    const int actions_y = y0 + row_h * 6 + 2;
    make_action(s_root, "wifi setup", 150, actions_y, 154, theme.accent, on_open_wifi);
    make_action(s_root, "calibrate", 318, actions_y, 150, theme.accent, on_open_touch_cal);

    const int web_y = actions_y + 50;
    make_label(s_root, "WEB", label_x, web_y + 9);
    web_who = make_label(s_root, "", value_x, web_y);
    lv_obj_set_width(web_who, LCD_W - value_x - 20);
    lv_label_set_long_mode(web_who, LV_LABEL_LONG_DOT);
    web_where = lv_label_create(s_root);
    style_value(web_where, &font_b612_14, theme.fg);
    lv_obj_set_pos(web_where, value_x, web_y + 19);
    lv_obj_set_width(web_where, LCD_W - value_x - 20);
    lv_label_set_long_mode(web_where, LV_LABEL_LONG_DOT);
    update_web_login();

    update_segment(seg_brightness, nearest_brightness_index(brightness()));
    update_brightness_label();

    // DAY / NIGHT only (no AUTO button here); AUTO and the other palettes
    // light no button.
    String theme_pref = current_theme_pref();
    int theme_idx = theme_choice::index_for(theme_pref.c_str());
    update_segment(seg_theme, theme_idx < (int)seg_theme.count ? theme_idx : -1);

    PosFormat fmt = pos_format();
    update_segment(seg_format, fmt == PosFormat::DDM ? 0 : fmt == PosFormat::DD ? 1 : 2);

    update_segment(seg_depth, nearest_double_index(depth_alarm_m(), DEPTH_VALUES, 5));
    update_depth_label();

    update_segment(seg_battery, nearest_double_index(battery_alarm_v(), BATTERY_VALUES, 5));
    update_battery_label();

#if YEYBOATS_ENABLE_DEMO
    update_segment(seg_demo, demo_enabled ? 1 : 0);
#endif

    return s_root;
}

void refresh() {
    // Settings update from their own events; only the web login row follows
    // the network (address, manager pairing), every ~2 s while visible.
    if (web_who && (uint32_t)(millis() - web_refreshed_ms) >= 2000) update_web_login();
}

}  // namespace ui::settings
