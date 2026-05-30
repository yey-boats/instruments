#include "screens.h"
#include "ui_theme.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "net.h"
#include "touch_cal.h"

#include <Arduino.h>

// Per docs/specs/05 + 06: touch calibration is a hidden screen invoked
// once from Settings (or POST /api/cmd "touch-cal"). Shows four
// crosshairs in sequence at the panel corners; user taps each in turn,
// raw GT911 coordinates are recorded, a 6-parameter affine is fit and
// persisted to NVS, then we return to Settings.

// Raw touch state exposed by main.cpp (pre-calibration).
extern "C" {
void main_touch_raw(int *raw_x, int *raw_y, int *pressed);
}

namespace ui::touch_cal_screen {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_target = nullptr;  // current crosshair widget
static lv_obj_t *s_lbl_step = nullptr;
static lv_obj_t *s_lbl_help = nullptr;
static lv_obj_t *s_btn_cancel = nullptr;
static lv_timer_t *s_timer = nullptr;

static constexpr int N_TARGETS = 4;
// Crosshair positions in screen coordinates - corners 40 px in from
// each edge so the user has room to tap near the center.
static constexpr int TX[N_TARGETS] = {40, LCD_W - 40, LCD_W - 40, 40};
static constexpr int TY[N_TARGETS] = {40, 40, LCD_H - 40, LCD_H - 40};

static int s_step = 0;
static ::touch_cal::Sample s_samples[N_TARGETS];
static bool s_last_pressed = false;
static int16_t s_press_rx = -1, s_press_ry = -1;
static uint32_t s_press_ms = 0;

static void move_target(int step) {
    if (!s_target) return;
    lv_obj_set_pos(s_target, TX[step] - 14, TY[step] - 14);
    char buf[24];
    snprintf(buf, sizeof(buf), "POINT %d/%d", step + 1, N_TARGETS);
    if (s_lbl_step) lv_label_set_text(s_lbl_step, buf);
}

static void apply_and_save() {
    ::touch_cal::Matrix m;
    if (!::touch_cal::solve(s_samples, N_TARGETS, m)) {
        net::logf("[cal] solve failed - keeping previous matrix");
        if (s_lbl_help) lv_label_set_text(s_lbl_help, "Solve failed - tap to retry");
        s_step = 0;
        move_target(0);
        return;
    }
    ::touch_cal::set(m);
    net::logf("[cal] calibrated %d points -> a=%.4f b=%.4f c=%.2f / d=%.4f e=%.4f f=%.2f",
              N_TARGETS, m.a, m.b, m.c, m.d, m.e, m.f);
    // Navigate immediately. Any deferred path (LVGL timer, "press again
    // to exit") risks getting stuck if lv_timer_handler stalls or the
    // user is confused about state.
    s_step = N_TARGETS + 1;
    ui::show_by_id("settings");
}

static void on_cancel(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_step = 0;
    ui::show_by_id("settings");
}

static void poll(lv_timer_t *) {
    // Capture press-release transitions of the raw touch.
    int rx = -1, ry = -1, pressed = 0;
    ::main_touch_raw(&rx, &ry, &pressed);

    // Emergency escape: a sustained press (>= 1.2 s) anywhere in the
    // bottom 100 px of the RAW panel area exits to Settings. Doesn't
    // depend on LVGL hit-testing - if the panel has a coordinate shift
    // big enough to break the cancel button, this still works because
    // it reads raw GT911 coords directly.
    if (pressed && s_last_pressed && s_press_ry >= 380 && (millis() - s_press_ms) >= 1200) {
        net::logf("[cal] emergency escape (raw bottom-zone long press)");
        s_step = N_TARGETS + 1;
        ui::show_by_id("settings");
        return;
    }

    if (pressed && !s_last_pressed) {
        // Press DOWN
        s_press_rx = rx;
        s_press_ry = ry;
        s_press_ms = millis();
    }
    if (!pressed && s_last_pressed) {
        // Press UP - count it as a sample for the current step if we
        // have a valid down coordinate and the press lasted >= 60 ms
        // (reject electrical glitches).
        uint32_t dt = millis() - s_press_ms;
        if (dt >= 60 && s_press_rx >= 0 && s_press_ry >= 0) {
            if (s_step >= 0 && s_step < N_TARGETS) {
                s_samples[s_step].raw_x = s_press_rx;
                s_samples[s_step].raw_y = s_press_ry;
                s_samples[s_step].target_x = TX[s_step];
                s_samples[s_step].target_y = TY[s_step];
                net::logf("[cal] point %d: raw=(%d,%d) target=(%d,%d)", s_step + 1, s_press_rx,
                          s_press_ry, TX[s_step], TY[s_step]);
                s_step++;
                if (s_step >= N_TARGETS) {
                    apply_and_save();
                } else {
                    move_target(s_step);
                }
            } else if (s_step == N_TARGETS + 1) {
                // Done sentinel - leave to Settings on the next press release.
                ui::show_by_id("settings");
            }
        }
        s_press_rx = -1;
        s_press_ry = -1;
    }
    s_last_pressed = pressed;
}

static lv_obj_t *make_target(lv_obj_t *parent) {
    // 28x28 transparent box with two crossed bars inside.
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 28, 28);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    // Horizontal bar
    lv_obj_t *h = lv_obj_create(box);
    lv_obj_set_size(h, 28, 3);
    lv_obj_set_pos(h, 0, 13);
    lv_obj_set_style_bg_color(h, lv_color_hex(ui::theme.accent), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

    // Vertical bar
    lv_obj_t *v = lv_obj_create(box);
    lv_obj_set_size(v, 3, 28);
    lv_obj_set_pos(v, 13, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(ui::theme.accent), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);

    return box;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(s_root, 0, 0);
    // Force solid dark background so the cal UI fully covers whatever
    // screen the user was on before. The bezel uses very dark navy
    // (overrides theme so the bright accents read clearly).
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000814), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "TOUCH CALIBRATION");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    s_lbl_step = lv_label_create(s_root);
    lv_label_set_text(s_lbl_step, "POINT 1/4");
    lv_obj_set_style_text_font(s_lbl_step, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_step, lv_color_hex(theme.fg), 0);
    lv_obj_align(s_lbl_step, LV_ALIGN_CENTER, 0, -20);

    s_lbl_help = lv_label_create(s_root);
    lv_label_set_text(s_lbl_help, "Tap each cross.  Long-press bottom of screen to escape.");
    lv_obj_set_style_text_font(s_lbl_help, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_help, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(s_lbl_help, LV_ALIGN_CENTER, 0, 8);

    // Cancel button - bottom center, large hit target so it works
    // before calibration is applied. Returns to Settings without
    // saving the in-progress matrix.
    s_btn_cancel = lv_button_create(s_root);
    lv_obj_set_size(s_btn_cancel, 220, 60);
    lv_obj_align(s_btn_cancel, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_set_style_bg_color(s_btn_cancel, lv_color_hex(0x803030), 0);
    lv_obj_set_style_bg_opa(s_btn_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_btn_cancel, 8, 0);
    lv_obj_add_event_cb(s_btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_btn_cancel);
    lv_label_set_text(lbl, "CANCEL");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl);

    s_target = make_target(s_root);
    move_target(0);

    return s_root;
}

void refresh() {
    // First time this screen becomes active, reset the state machine
    // and stand up a 50 ms LVGL timer for polling raw touch coords.
    // We invalidate s_root once explicitly because lv_screen_load()
    // does not always trigger a full redraw if the previous active
    // screen was the same object.
    if (!s_timer) {
        s_timer = lv_timer_create(poll, 50, NULL);
    }
    // If we re-entered (e.g., user opened cal twice), reset state.
    if (lv_screen_active() == s_root) {
        if (s_step >= N_TARGETS) {
            // Coming back after a previous successful or failed run -
            // reset so the user can recalibrate.
            s_step = 0;
            s_last_pressed = false;
            move_target(0);
            if (s_lbl_help) lv_label_set_text(s_lbl_help, "Tap the highlighted cross");
        }
        lv_obj_invalidate(s_root);
    }
}

}  // namespace ui::touch_cal_screen
