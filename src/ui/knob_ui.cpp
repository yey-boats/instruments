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
#include "midl_store.h"
#include "screens.h"
#include "signalk.h"
#include "ui_data.h"
#include "ui_theme.h"

// =====================================================================
//  Dispatch core: knob events -> knob_menu step -> Action -> MIDL screen
//  navigation / SignalK PUT / remote view switch / overlay toggle. Runs
//  entirely on the UI task (called from app::pump()), so it is safe to
//  mutate LVGL here.
//
//  The knob shows its embedded round-360 MIDL records (ap_hud,
//  knob_compass, knob_wind, knob_big). The rotary menu is system UI on
//  lv_layer_top() (like settings on the square boards): Home turns
//  through the MIDL carousel; Menu/Steer/ModePicker/Displays are overlay
//  levels (see knob_menu.h).
// =====================================================================

namespace knob_ui {

namespace {

knob::Model s_model;

knob::Inputs snapshot_inputs() {
    knob::Inputs in;
    static char state_buf[16];
    boat::View d;
    boat::current_view(d);
    strncpy(state_buf, d.apState[0] ? d.apState : "", sizeof(state_buf) - 1);
    state_buf[sizeof(state_buf) - 1] = 0;
    in.ap_state = state_buf;
    in.ap_target_rad = d.apTargetHdg;
    in.heading_rad = d.headingTrue;
    in.display_count = knob_remote::display_count();
    if (s_model.level == knob::Level::SelectView) {
        knob_remote::DisplayEntry e;
        if (knob_remote::copy_entry(s_model.entered_display, e)) in.view_count = e.view_count;
    }
    return in;
}

// Build + post the SignalK PUT for an autopilot Action, formatting the path
// and JSON value via the pure knob::signalk_put_for so the device and the host
// tests share ONE formatting source. Returns true if a PUT was posted.
bool post_ap_put(const knob::Action &a) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    if (!knob::signalk_put_for(a, cmd.a, sizeof(cmd.a), cmd.b, sizeof(cmd.b))) return false;
    app::post_net(cmd, 50);
    if (a.type == knob::ActionType::ApSetState)
        net::logf("[knob] state -> %s", a.arg_str);
    else
        net::logf("[knob] target -> %.0f deg", a.arg_f * 180.0 / M_PI);
    return true;
}

void perform(const knob::Action &a) {
    switch (a.type) {
    case knob::ActionType::ApSetState:
    case knob::ActionType::ApSetTargetRad:
        post_ap_put(a);
        break;
    case knob::ActionType::SwitchView:
        knob_remote::switch_view(a.arg_dev_idx, a.arg_view_idx);
        break;
    case knob::ActionType::NextScreen:
        midl::store_ui::next();
        break;
    case knob::ActionType::PrevScreen:
        midl::store_ui::prev();
        break;
    case knob::ActionType::HomeScreen:
        midl::store_ui::home();
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

// Steer level: the autopilot HUD is the screen being steered, so show the
// ap_hud MIDL screen (a no-op when no record owns it) and keep the overlay
// hidden; a small STEER badge says the knob now moves the AP target.
static void on_enter_steer() {
    midl::store_ui::show("ap_hud");
}

void apply_event(int ev, bool held) {
    knob::Level prev = s_model.level;
    int prev_display = s_model.entered_display;
    knob::Inputs in = snapshot_inputs();
    knob::Action a = knob::step(s_model, in, (knob::Event)ev, held);
    perform(a);
    // On drill-in to SelectView (or switching which display is entered), ask
    // the manager worker to lazily fetch that display's view list. The fetch
    // is blocking HTTP, so it runs off the UI task; this only sets a flag.
    if (s_model.level == knob::Level::SelectView &&
        (prev != knob::Level::SelectView || prev_display != s_model.entered_display)) {
        knob_remote::request_views_fetch(s_model.entered_display);
    }
    if (s_model.level == knob::Level::Steer && prev != knob::Level::Steer) on_enter_steer();
    // The list overlay covers the screen on the menu levels only: Home shows
    // the MIDL screen, Steer shows the ap_hud screen + the STEER badge.
    ui::knob_menu_overlay::show(s_model.level != knob::Level::Home &&
                                s_model.level != knob::Level::Steer);
    ui::knob_menu_overlay::refresh();
}

}  // namespace knob_ui

// ---------------- knob_menu_overlay: list on lv_layer_top() ---------------
namespace ui::knob_menu_overlay {

static lv_obj_t *s_root = nullptr;   // overlay container on layer_top
static lv_obj_t *s_title = nullptr;  // level title
static lv_obj_t *s_badge = nullptr;  // "STEER" chip while the knob moves the AP target
static constexpr int kMaxRows = 6;
static lv_obj_t *s_rows[kMaxRows] = {nullptr};

lv_obj_t *build(lv_obj_t * /*parent*/) {
    if (s_root) return s_root;
    lv_obj_t *top = lv_layer_top();

    s_root = lv_obj_create(top);
    // Backdrop that fills the WHOLE panel with the theme ground: full
    // opacity, no corner radius (a rounded LV_OPA_90 circle left the corners
    // un-painted and dimmed the fill). Content stays inside the usable
    // inscribed rect.
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_root);
    lv_label_set_text(s_title, "MENU");
    lv_obj_set_style_text_font(s_title, &font_b612_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(theme.warn), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, board::geometry().usable_y + 6);

    for (int i = 0; i < kMaxRows; ++i) {
        lv_obj_t *r = lv_label_create(s_root);
        lv_label_set_text(r, "");
        lv_obj_set_style_text_font(r, &font_b612_20, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(theme.fg), 0);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, board::geometry().usable_y + 40 + i * 30);
        s_rows[i] = r;
    }

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    // Separate from s_root (which hides on Steer): a small chip at the top of
    // the round panel, inside the usable rect.
    s_badge = lv_label_create(top);
    lv_label_set_text(s_badge, " STEER ");
    lv_obj_set_style_text_font(s_badge, &font_b612_14, 0);
    lv_obj_set_style_text_color(s_badge, lv_color_hex(theme.button_active_fg), 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(theme.button_active_bg), 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_badge, 4, 0);
    lv_obj_set_style_pad_all(s_badge, 3, 0);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, board::geometry().usable_y + 4);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
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

// Compute the first list index to render so a `kMaxRows`-tall window always
// keeps `highlight` visible. Lists shorter than the window start at 0; longer
// lists scroll so the highlight stays on screen (and the window never runs past
// the end of the list).
static int window_start(int count, int highlight) {
    if (count <= kMaxRows) return 0;
    int start = highlight - kMaxRows / 2;
    if (start < 0) start = 0;
    if (start > count - kMaxRows) start = count - kMaxRows;
    return start;
}

void refresh() {
    if (!s_root) return;
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    const knob::Model &m = knob_ui::model();
    char buf[48];

    if (s_badge) {
        if (m.level == knob::Level::Steer)
            lv_obj_clear_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    }

    switch (m.level) {
    case knob::Level::Menu: {
        lv_label_set_text(s_title, "MENU");
        for (int i = 0; i < kMaxRows; ++i) {
            if (i < knob::kMenuCount)
                set_row(i, knob::menu_item_label(i), i == m.highlight);
            else
                set_row(i, "", false);
        }
        break;
    }
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
        int start = window_start(n, m.highlight);
        for (int row = 0; row < kMaxRows; ++row) {
            int idx = start + row;
            if (idx < n) {
                knob_remote::DisplayEntry e;
                bool ok = knob_remote::copy_entry(idx, e);
                snprintf(buf, sizeof(buf), "#%d %s", idx, ok ? e.name : "?");
                set_row(row, buf, idx == m.highlight);
            } else {
                set_row(row, "", false);
            }
        }
        break;
    }
    case knob::Level::SelectView: {
        lv_label_set_text(s_title, "VIEW");
        knob_remote::DisplayEntry e;
        bool ok = knob_remote::copy_entry(m.entered_display, e);
        int n = ok ? e.view_count : 0;
        int start = window_start(n, m.highlight);
        for (int row = 0; row < kMaxRows; ++row) {
            int idx = start + row;
            if (idx < n) {
                bool cur = (idx == e.current_view);
                snprintf(buf, sizeof(buf), "%s%s", e.view_title[idx], cur ? " *" : "");
                set_row(row, buf, idx == m.highlight);
            } else {
                set_row(row, "", false);
            }
        }
        break;
    }
    case knob::Level::Home:
    case knob::Level::Steer:
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
