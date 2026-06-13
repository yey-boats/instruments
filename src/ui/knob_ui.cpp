#include "knob_ui.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

#include <ctype.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "board.h"
#include "board_pins.h"
#include "knob_menu.h"
#include "knob_remote.h"
#include "net.h"
#include "screens.h"
#include "signalk.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_theme.h"

// =====================================================================
//  Dispatch core: knob events -> knob_menu step -> Action -> SignalK PUT
//  / remote view switch / overlay toggle. Runs entirely on the UI task
//  (called from app::pump()), so it is safe to mutate LVGL here.
// =====================================================================

namespace knob_ui {

namespace {

knob::Model s_model;

knob::Inputs snapshot_inputs() {
    knob::Inputs in;
    static char state_buf[16];
    sk::Data d;
    sk::copyData(d);
    strncpy(state_buf, d.apState[0] ? d.apState : "", sizeof(state_buf) - 1);
    state_buf[sizeof(state_buf) - 1] = 0;
    in.ap_state = state_buf;
    in.ap_target_rad = d.apTargetHdg;
    in.heading_rad = d.headingTrue;
    in.display_count = knob_remote::display_count();
    const knob_remote::DisplayEntry *e = s_model.level == knob::Level::SelectView
                                             ? knob_remote::display_at(s_model.entered_display)
                                             : nullptr;
    in.view_count = e ? e->view_count : 0;
    return in;
}

void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[knob] state -> %s", state);
}

void put_target(double rad) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/target/headingTrue", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "%.4f", rad);
    app::post_net(cmd, 50);
    net::logf("[knob] target -> %.0f deg", rad * 180.0 / M_PI);
}

void perform(const knob::Action &a) {
    switch (a.type) {
    case knob::ActionType::ApSetState:
        put_state(a.arg_str);
        break;
    case knob::ActionType::ApSetTargetRad:
        put_target(a.arg_f);
        break;
    case knob::ActionType::SwitchView:
        knob_remote::switch_view(a.arg_dev_idx, a.arg_view_idx);
        break;
    case knob::ActionType::NoAction:
        break;
    }
}

}  // namespace

const knob::Model &model() {
    return s_model;
}

void setup() {
    knob::init(s_model);
    knob_remote::setup();
    ui::knob_menu_overlay::build(nullptr);
    ui::knob_menu_overlay::show(false);
}

void apply_event(int ev, bool held) {
    knob::Inputs in = snapshot_inputs();
    knob::Action a = knob::step(s_model, in, (knob::Event)ev, held);
    perform(a);
    // Overlay visible whenever we're not on the autopilot Home level.
    ui::knob_menu_overlay::show(s_model.level != knob::Level::Home);
    ui::knob_menu_overlay::refresh();
}

}  // namespace knob_ui

// =====================================================================
//  Round views. All content stays inside the inscribed usable_* rect so
//  nothing clips on the round panel. Follows the screen_autopilot.cpp
//  pattern: build() once, refresh() with dirty-value caches.
// =====================================================================

namespace {

// Common round-screen root: full panel, themed bg, no scroll.
lv_obj_t *make_round_root(lv_obj_t *parent) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(ui::theme.bg), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

}  // namespace

// ---------------- ap_hud: mode badge + big target + HDG + delta ----------
namespace ui::ap_hud {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_state, *lbl_target, *lbl_hdg, *lbl_delta;

static const char *kModeState[4] = {"standby", "auto", "wind", "route"};

lv_obj_t *build(lv_obj_t *parent) {
    s_root = make_round_root(parent);

    lbl_state = lv_label_create(s_root);
    lv_label_set_text(lbl_state, "STANDBY");
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_state, LV_ALIGN_CENTER, 0, -90);

    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "TARGET");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -48);

    lbl_target = lv_label_create(s_root);
    lv_label_set_text(lbl_target, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_target, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_target, lv_color_hex(theme.warn), 0);
    lv_obj_align(lbl_target, LV_ALIGN_CENTER, 0, -8);

    lbl_hdg = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg, "HDG ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_CENTER, -50, 56);

    lbl_delta = lv_label_create(s_root);
    lv_label_set_text(lbl_delta, "\xCE\x94 ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_delta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_delta, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_delta, LV_ALIGN_CENTER, 50, 56);

    return s_root;
}

static char s_last_state[16] = {(char)0xFF};
static char s_last_target[16] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_delta[16] = {(char)0xFF};
static uint32_t s_last_state_color = 0xFFFFFFFF;

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[32];

    int active = -1;
    for (int i = 0; i < 4; ++i)
        if (d.apState[0] && strcmp(d.apState, kModeState[i]) == 0) active = i;

    if (d.apState[0]) {
        char up[16];
        size_t i = 0;
        for (; d.apState[i] && i < sizeof(up) - 1; ++i)
            up[i] = (char)toupper((unsigned char)d.apState[i]);
        up[i] = 0;
        set_text_if_changed(lbl_state, s_last_state, sizeof(s_last_state), up);
        set_text_color_if_changed(lbl_state, &s_last_state_color,
                                  active >= 1 ? theme.good : theme.fg_dim);
    } else {
        set_text_if_changed(lbl_state, s_last_state, sizeof(s_last_state), "OFFLINE");
        set_text_color_if_changed(lbl_state, &s_last_state_color, theme.fg_dim);
    }

    double target = d.apTargetHdg;
    if (!isnan(target)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(target));
        set_text_if_changed(lbl_target, s_last_target, sizeof(s_last_target), buf);
    } else {
        set_text_if_changed(lbl_target, s_last_target, sizeof(s_last_target), "---\xC2\xB0");
    }

    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "HDG %03.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        set_text_if_changed(lbl_hdg, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg, s_last_hdg, sizeof(s_last_hdg), "HDG ---\xC2\xB0");
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
}

}  // namespace ui::ap_hud

// ---------------- knob_compass: heading + COG -----------------------------
namespace ui::knob_compass {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_hdg, *lbl_cog;

lv_obj_t *build(lv_obj_t *parent) {
    s_root = make_round_root(parent);

    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "HEADING");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -64);

    lbl_hdg = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_hdg, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_hdg, LV_ALIGN_CENTER, 0, -16);

    lbl_cog = lv_label_create(s_root);
    lv_label_set_text(lbl_cog, "COG ---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_cog, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cog, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_cog, LV_ALIGN_CENTER, 0, 56);

    return s_root;
}

static char s_last_hdg[16] = {(char)0xFF};
static char s_last_cog[16] = {(char)0xFF};

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[32];

    if (!isnan(d.headingTrue)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", rad_to_deg_pos(d.headingTrue));
        set_text_if_changed(lbl_hdg, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg, s_last_hdg, sizeof(s_last_hdg), "---\xC2\xB0");
    }

    if (!isnan(d.cogTrue)) {
        snprintf(buf, sizeof(buf), "COG %03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
        set_text_if_changed(lbl_cog, s_last_cog, sizeof(s_last_cog), buf);
    } else {
        set_text_if_changed(lbl_cog, s_last_cog, sizeof(s_last_cog), "COG ---\xC2\xB0");
    }
}

}  // namespace ui::knob_compass

// ---------------- knob_wind: AWS + apparent angle -------------------------
namespace ui::knob_wind {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_aws, *lbl_awa;

lv_obj_t *build(lv_obj_t *parent) {
    s_root = make_round_root(parent);

    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "APP WIND");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -64);

    lbl_aws = lv_label_create(s_root);
    lv_label_set_text(lbl_aws, "--.- kn");
    lv_obj_set_style_text_font(lbl_aws, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_aws, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_aws, LV_ALIGN_CENTER, 0, -16);

    lbl_awa = lv_label_create(s_root);
    lv_label_set_text(lbl_awa, "--\xC2\xB0");
    lv_obj_set_style_text_font(lbl_awa, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_awa, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_awa, LV_ALIGN_CENTER, 0, 56);

    return s_root;
}

static char s_last_aws[16] = {(char)0xFF};
static char s_last_awa[16] = {(char)0xFF};

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[32];

    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.aws));
        set_text_if_changed(lbl_aws, s_last_aws, sizeof(s_last_aws), buf);
    } else {
        set_text_if_changed(lbl_aws, s_last_aws, sizeof(s_last_aws), "--.- kn");
    }

    if (!isnan(d.awa)) {
        double a = rad_to_deg_pm(d.awa);
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 %s", fabs(a), a >= 0 ? "STBD" : "PORT");
        set_text_if_changed(lbl_awa, s_last_awa, sizeof(s_last_awa), buf);
    } else {
        set_text_if_changed(lbl_awa, s_last_awa, sizeof(s_last_awa), "--\xC2\xB0");
    }
}

}  // namespace ui::knob_wind

// ---------------- knob_big: one large value (depth) -----------------------
namespace ui::knob_big {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_cap, *lbl_value, *lbl_unit;

lv_obj_t *build(lv_obj_t *parent) {
    s_root = make_round_root(parent);

    lbl_cap = lv_label_create(s_root);
    lv_label_set_text(lbl_cap, "DEPTH");
    lv_obj_set_style_text_font(lbl_cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_cap, LV_ALIGN_CENTER, 0, -72);

    lbl_value = lv_label_create(s_root);
    lv_label_set_text(lbl_value, "--.-");
    lv_obj_set_style_text_font(lbl_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_value, lv_color_hex(theme.accent), 0);
    lv_obj_align(lbl_value, LV_ALIGN_CENTER, 0, 0);

    lbl_unit = lv_label_create(s_root);
    lv_label_set_text(lbl_unit, "m");
    lv_obj_set_style_text_font(lbl_unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_unit, LV_ALIGN_CENTER, 0, 64);

    return s_root;
}

static char s_last_value[16] = {(char)0xFF};

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[16];
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        set_text_if_changed(lbl_value, s_last_value, sizeof(s_last_value), buf);
    } else {
        set_text_if_changed(lbl_value, s_last_value, sizeof(s_last_value), "--.-");
    }
}

}  // namespace ui::knob_big

// ---------------- knob_menu_overlay: list on lv_layer_top() ---------------
namespace ui::knob_menu_overlay {

static lv_obj_t *s_root = nullptr;   // overlay container on layer_top
static lv_obj_t *s_title = nullptr;  // level title
static constexpr int kMaxRows = 6;
static lv_obj_t *s_rows[kMaxRows] = {nullptr};

lv_obj_t *build(lv_obj_t * /*parent*/) {
    if (s_root) return s_root;
    lv_obj_t *top = lv_layer_top();

    s_root = lv_obj_create(top);
    // Round backdrop that fills the panel; content kept in the usable rect.
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_root, LCD_W / 2, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_root);
    lv_label_set_text(s_title, "MENU");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(theme.warn), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, board::geometry().usable_y + 6);

    for (int i = 0; i < kMaxRows; ++i) {
        lv_obj_t *r = lv_label_create(s_root);
        lv_label_set_text(r, "");
        lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(theme.fg), 0);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, board::geometry().usable_y + 40 + i * 30);
        s_rows[i] = r;
    }

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    return s_root;
}

void show(bool on) {
    if (!s_root) return;
    if (on)
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

static void set_row(int i, const char *text, bool highlight) {
    if (i < 0 || i >= kMaxRows || !s_rows[i]) return;
    lv_label_set_text(s_rows[i], text ? text : "");
    lv_obj_set_style_text_color(s_rows[i], lv_color_hex(highlight ? theme.accent : theme.fg_dim),
                                0);
}

void refresh() {
    if (!s_root) return;
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    const knob::Model &m = knob_ui::model();
    char buf[48];

    switch (m.level) {
    case knob::Level::ModePicker: {
        lv_label_set_text(s_title, "MODE");
        static const char *kModeName[knob::kModeCount] = {"STANDBY", "COMPASS", "WIND", "ROUTE"};
        for (int i = 0; i < kMaxRows; ++i) {
            if (i < knob::kModeCount)
                set_row(i, kModeName[i], i == m.highlight);
            else
                set_row(i, "", false);
        }
        break;
    }
    case knob::Level::SelectDisplay: {
        lv_label_set_text(s_title, "DISPLAY");
        int n = knob_remote::display_count();
        for (int i = 0; i < kMaxRows; ++i) {
            if (i < n) {
                const knob_remote::DisplayEntry *e = knob_remote::display_at(i);
                snprintf(buf, sizeof(buf), "#%d %s", i, e ? e->name : "?");
                set_row(i, buf, i == m.highlight);
            } else {
                set_row(i, "", false);
            }
        }
        break;
    }
    case knob::Level::SelectView: {
        lv_label_set_text(s_title, "VIEW");
        const knob_remote::DisplayEntry *e = knob_remote::display_at(m.entered_display);
        int n = e ? e->view_count : 0;
        for (int i = 0; i < kMaxRows; ++i) {
            if (e && i < n) {
                bool cur = (i == e->current_view);
                snprintf(buf, sizeof(buf), "%s%s", e->view_title[i], cur ? " *" : "");
                set_row(i, buf, i == m.highlight);
            } else {
                set_row(i, "", false);
            }
        }
        break;
    }
    case knob::Level::Home:
    default:
        lv_label_set_text(s_title, "");
        for (int i = 0; i < kMaxRows; ++i)
            set_row(i, "", false);
        break;
    }
#endif
}

}  // namespace ui::knob_menu_overlay

#else  // not the knob board

namespace knob_ui {
void setup() {
}
void apply_event(int, bool) {
}
}  // namespace knob_ui

#endif  // BOARD_ID_WAVESHARE_KNOB_1_8
