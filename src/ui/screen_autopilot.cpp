#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "signalk.h"
#include "net.h"
#include "board_pins.h"
#include "app_events.h"

#include <math.h>
#include <stdio.h>

// Fullscreen steering page. Selects the autopilot steering MODE and tweaks the
// target. Modes map to the autopilot backend (KDCube sim via the plugin
// bridge) over SignalK:
//   STANDBY  -> "standby" (disengaged, hold helm)
//   COMPASS  -> "auto"    (hold a heading; +/- buttons adjust it)
//   WIND     -> "wind"    (hold the apparent wind angle)
//   ROUTE    -> "route"   (follow the active route)
//   -10 -1 +1 +10         adjust target heading (compass) by N degrees
//
// PUT uses the net worker (sk::putValue is too slow for the UI handler).

namespace ui::autopilot {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_state, *lbl_target_value, *lbl_hdg_value, *lbl_delta, *lbl_status;
static lv_obj_t *mode_btn[4];
static lv_obj_t *mode_lbl[4];

// Mode button order + the SignalK state string each commits.
static const char *kModeName[4] = {"STANDBY", "COMPASS", "WIND", "ROUTE"};
static const char *kModeState[4] = {"standby", "auto", "wind", "route"};

static double s_target_local = NAN;  // local pending target heading (rad)

static void put_heading(double rad) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/target/headingTrue", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "%.4f", rad);
    app::post_net(cmd, 50);
    net::logf("[ap] target -> %.0f\xC2\xB0 queued", rad * 180.0 / M_PI);
}

static void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[ap] state -> %s queued", state);
}

static void on_mode(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 3) return;
    if (idx != 1) s_target_local = NAN;  // leaving compass clears local target
    put_state(kModeState[idx]);
}

static void on_adjust(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta_deg = (int)(intptr_t)lv_event_get_user_data(e);
    double target = s_target_local;
    if (isnan(target)) {
        sk::Data d_snap;
        sk::copyData(d_snap);
        target = d_snap.apTargetHdg;
        if (isnan(target)) target = d_snap.headingTrue;
    }
    if (isnan(target)) target = 0;
    target += delta_deg * M_PI / 180.0;
    while (target < 0)
        target += 2 * M_PI;
    while (target >= 2 * M_PI)
        target -= 2 * M_PI;
    s_target_local = target;
    put_heading(target);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, int x, int y, int w, int h,
                             uint32_t color, lv_event_cb_t cb, intptr_t udata, lv_obj_t **out_lbl,
                             const lv_font_t *font) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, ui::chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)udata);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0x05101c), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
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

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "STEERING");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // State badge (engaged mode, coloured).
    lbl_state = lv_label_create(s_root);
    lv_label_set_text(lbl_state, "STANDBY");
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_MID, 0, 34);

    // Target hero.
    lv_obj_t *cap_target = lv_label_create(s_root);
    lv_label_set_text(cap_target, "TARGET");
    lv_obj_set_style_text_font(cap_target, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap_target, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap_target, LV_ALIGN_TOP_MID, 0, 78);

    lbl_target_value = lv_label_create(s_root);
    lv_label_set_text(lbl_target_value, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_target_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_target_value, lv_color_hex(theme.warn), 0);
    lv_obj_align(lbl_target_value, LV_ALIGN_TOP_MID, 0, 96);

    // Current HDG + delta.
    lbl_hdg_value = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg_value, "HDG ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hdg_value, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_hdg_value, LV_ALIGN_TOP_MID, -64, 158);

    lbl_delta = lv_label_create(s_root);
    lv_label_set_text(lbl_delta, "\xCE\x94 ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_delta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_delta, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_delta, LV_ALIGN_TOP_MID, 64, 158);

    // Heading-adjust row (compass mode): -10 -1 +1 +10.
    int btn_w = (LCD_W - 40) / 4;
    int adj_y = 196;
    make_button(s_root, "-10", 8, adj_y, btn_w, 56, theme.port, on_adjust, -10, nullptr,
                &lv_font_montserrat_28);
    make_button(s_root, "-1", 16 + btn_w, adj_y, btn_w, 56, theme.port, on_adjust, -1, nullptr,
                &lv_font_montserrat_28);
    make_button(s_root, "+1", 24 + btn_w * 2, adj_y, btn_w, 56, theme.starboard, on_adjust, +1,
                nullptr, &lv_font_montserrat_28);
    make_button(s_root, "+10", 32 + btn_w * 3, adj_y, btn_w, 56, theme.starboard, on_adjust, +10,
                nullptr, &lv_font_montserrat_28);

    // Mode row: STANDBY | COMPASS | WIND | ROUTE (2x2 so labels fit big).
    int mw = (LCD_W - 24) / 2;
    int mh = 64;
    int my0 = 268;
    for (int i = 0; i < 4; ++i) {
        int col = i % 2, row = i / 2;
        int x = 8 + col * (mw + 8);
        int y = my0 + row * (mh + 8);
        mode_btn[i] = make_button(s_root, kModeName[i], x, y, mw, mh, theme.panel_edge, on_mode,
                                  (intptr_t)i, &mode_lbl[i], &lv_font_montserrat_28);
    }

    lbl_status = lv_label_create(s_root);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -4);

    return s_root;
}

// Dirty-value caches.
static char s_last_state[16] = {(char)0xFF};
static char s_last_target[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_delta[16] = {(char)0xFF};
static char s_last_status[24] = {(char)0xFF};
static uint32_t s_last_state_color = 0xFFFFFFFF;
static int s_last_active_mode = -2;

void refresh() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    // Active mode index from the live AP state.
    int active = -1;
    for (int i = 0; i < 4; ++i)
        if (d.apState[0] && strcmp(d.apState, kModeState[i]) == 0) active = i;

    if (d.apState[0]) {
        char up[16];
        size_t i = 0;
        for (; d.apState[i] && i < sizeof(up) - 1; ++i)
            up[i] = toupper(d.apState[i]);
        up[i] = 0;
        set_text_if_changed(lbl_state, s_last_state, sizeof(s_last_state), up);
        bool engaged = active >= 1;  // anything but standby/unknown
        set_text_color_if_changed(lbl_state, &s_last_state_color,
                                  engaged ? theme.good : theme.fg_dim);
    } else {
        set_text_if_changed(lbl_state, s_last_state, sizeof(s_last_state), "OFFLINE");
        set_text_color_if_changed(lbl_state, &s_last_state_color, theme.fg_dim);
    }

    // Highlight the active mode button.
    if (active != s_last_active_mode) {
        for (int i = 0; i < 4; ++i) {
            bool on = (i == active);
            lv_obj_set_style_bg_color(mode_btn[i],
                                      lv_color_hex(on ? theme.accent : theme.panel_edge), 0);
            lv_obj_set_style_text_color(mode_lbl[i], lv_color_hex(on ? 0x05101c : theme.fg), 0);
        }
        s_last_active_mode = active;
    }

    double target = !isnan(s_target_local) ? s_target_local : d.apTargetHdg;
    if (!isnan(target)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(target));
        set_text_if_changed(lbl_target_value, s_last_target, sizeof(s_last_target), buf);
    } else {
        set_text_if_changed(lbl_target_value, s_last_target, sizeof(s_last_target), "---\xC2\xB0");
    }

    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "HDG %03.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), "HDG ---\xC2\xB0");
    }

    if (!isnan(target) && !isnan(d.headingTrue)) {
        double delta = (target - d.headingTrue) * 180.0 / M_PI;
        while (delta > 180)
            delta -= 360;
        while (delta < -180)
            delta += 360;
        snprintf(buf, sizeof(buf), "\xCE\x94 %+.0f\xC2\xB0", delta);
        set_text_if_changed(lbl_delta, s_last_delta, sizeof(s_last_delta), buf);
    } else {
        set_text_if_changed(lbl_delta, s_last_delta, sizeof(s_last_delta), "\xCE\x94 ---\xC2\xB0");
    }

    String sk_state = sk::connectionStatus();
    const char *status = (sk_state == "live")      ? "SignalK live"
                         : (sk_state == "no-data") ? "SignalK no data"
                                                   : sk_state.c_str();
    set_text_if_changed(lbl_status, s_last_status, sizeof(s_last_status), status);
}

}  // namespace ui::autopilot
