#include "screens.h"
#include "ui_theme.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "net.h"
#include "touch_cal.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

// 10x10 touch grid screen with two modes:
//
//   TEST  - user taps anywhere; the cell that maps from the current
//           calibration lights up. Pure touch-coverage check; shows
//           where the active matrix actually lands vs where the user
//           intended. No calibration is saved.
//
//   CAL   - one cell at a time is highlighted in amber; the user taps
//           wherever they see the highlight, the raw GT911 sample is
//           recorded against the highlighted cell's known target. After
//           >= 12 cells, APPLY fits a 6-parameter affine to the
//           captures and persists it via touch_cal.
//
// Modes are switched via the bottom-row buttons.
//
// Reachable via console: `touch-grid` / `grid-cal`.

extern "C" {
void main_touch_raw(int *raw_x, int *raw_y, int *pressed);
}

namespace ui::touch_grid_screen {

static constexpr int N = 10;
static constexpr int CELLS = N * N;
static constexpr int MIN_FOR_FIT = 12;

enum class Mode : uint8_t { Test, Cal };

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_cells[CELLS] = {nullptr};
static lv_obj_t *s_lbl_status = nullptr;
static lv_obj_t *s_lbl_help = nullptr;
static lv_obj_t *s_btn_left = nullptr;  // "START CAL" / "APPLY"
static lv_obj_t *s_btn_left_lbl = nullptr;
static lv_obj_t *s_btn_right = nullptr;  // "CANCEL" / "BACK TO TEST"
static lv_obj_t *s_btn_right_lbl = nullptr;
static lv_timer_t *s_timer = nullptr;

static Mode s_mode = Mode::Test;
static ::touch_cal::Sample s_samples[CELLS];
static int s_captured = 0;
static int s_active = 0;  // CAL mode: current walking-target index

static bool s_last_pressed = false;
static int16_t s_press_rx = -1, s_press_ry = -1;
static uint32_t s_press_ms = 0;

static int cell_w() {
    return LCD_W / N;
}
static int cell_h() {
    return LCD_H / N;
}
static int target_x_for_col(int c) {
    return c * cell_w() + cell_w() / 2;
}
static int target_y_for_row(int r) {
    return r * cell_h() + cell_h() / 2;
}

static int cell_for_screen(int sx, int sy) {
    if (sx < 0 || sy < 0 || sx >= LCD_W || sy >= LCD_H) return -1;
    int c = sx / cell_w();
    int r = sy / cell_h();
    if (c >= N) c = N - 1;
    if (r >= N) r = N - 1;
    return r * N + c;
}

static void paint_cell(int idx) {
    if (idx < 0 || idx >= CELLS || !s_cells[idx]) return;
    bool active_target = (s_mode == Mode::Cal && idx == s_active);
    bool filled = (s_samples[idx].raw_x >= 0);
    uint32_t bg, border;
    int border_w;
    if (active_target) {
        bg = 0xf6a21a;  // amber - walking target
        border = 0xffffff;
        border_w = 3;
    } else if (filled) {
        bg = 0x1b6e3a;  // dark green - captured
        border = 0x344050;
        border_w = 1;
    } else {
        bg = 0x1c2632;  // dim - empty
        border = 0x344050;
        border_w = 1;
    }
    lv_obj_set_style_bg_color(s_cells[idx], lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s_cells[idx], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cells[idx], lv_color_hex(border), 0);
    lv_obj_set_style_border_width(s_cells[idx], border_w, 0);
}

static void repaint_all() {
    for (int i = 0; i < CELLS; ++i)
        paint_cell(i);
}

static void set_active(int idx) {
    int prev = s_active;
    s_active = idx;
    if (prev >= 0 && prev < CELLS) paint_cell(prev);
    if (idx >= 0 && idx < CELLS) paint_cell(idx);
}

static void update_buttons() {
    if (s_mode == Mode::Test) {
        lv_label_set_text(s_btn_left_lbl, "START CAL");
        lv_obj_set_style_bg_color(s_btn_left, lv_color_hex(0x57c7d8), 0);
        lv_label_set_text(s_btn_right_lbl, "CANCEL");
        lv_obj_set_style_bg_color(s_btn_right, lv_color_hex(0x803030), 0);
    } else {
        if (s_captured >= MIN_FOR_FIT) {
            lv_label_set_text(s_btn_left_lbl, "APPLY");
            lv_obj_set_style_bg_color(s_btn_left, lv_color_hex(0x1b6e3a), 0);
        } else {
            char b[24];
            snprintf(b, sizeof(b), "%d more", MIN_FOR_FIT - s_captured);
            lv_label_set_text(s_btn_left_lbl, b);
            lv_obj_set_style_bg_color(s_btn_left, lv_color_hex(0x3b4250), 0);
        }
        lv_label_set_text(s_btn_right_lbl, "BACK");
        lv_obj_set_style_bg_color(s_btn_right, lv_color_hex(0x803030), 0);
    }
}

static void update_status() {
    if (!s_lbl_status) return;
    char buf[64];
    if (s_mode == Mode::Test) {
        snprintf(buf, sizeof(buf), "TEST   tap any cell - cell where touch lands lights up");
    } else {
        snprintf(buf, sizeof(buf), "CAL %d/%d  tap the AMBER cell", s_captured, CELLS);
    }
    lv_label_set_text(s_lbl_status, buf);
    update_buttons();
}

static void clear_captures() {
    s_captured = 0;
    for (int i = 0; i < CELLS; ++i) {
        s_samples[i].raw_x = -1;
        s_samples[i].raw_y = -1;
    }
    repaint_all();
}

static void enter_test() {
    s_mode = Mode::Test;
    clear_captures();
    update_status();
}

static void enter_cal() {
    s_mode = Mode::Cal;
    clear_captures();
    s_active = 0;
    repaint_all();
    update_status();
}

static void apply_and_save() {
    ::touch_cal::Sample fit[CELLS];
    int n = 0;
    for (int i = 0; i < CELLS; ++i) {
        if (s_samples[i].raw_x >= 0) fit[n++] = s_samples[i];
    }
    if (n < MIN_FOR_FIT) return;
    ::touch_cal::Matrix m;
    if (!::touch_cal::solve(fit, n, m)) {
        if (s_lbl_status)
            lv_label_set_text(s_lbl_status, "Solve rejected - tap cells more precisely");
        return;
    }
    ::touch_cal::set(m);
    net::logf("[cal-grid] applied affine from %d samples: "
              "a=%.4f b=%.4f c=%.2f d=%.4f e=%.4f f=%.2f",
              n, m.a, m.b, m.c, m.d, m.e, m.f);
    ui::show_by_id("settings");
}

static void on_left(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_mode == Mode::Test) {
        enter_cal();
    } else if (s_captured >= MIN_FOR_FIT) {
        apply_and_save();
    }
}

static void on_right(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_mode == Mode::Test) {
        ui::show_by_id("settings");
    } else {
        enter_test();
    }
}

static void poll(lv_timer_t *) {
    int rx = -1, ry = -1, pressed = 0;
    ::main_touch_raw(&rx, &ry, &pressed);

    if (pressed && !s_last_pressed) {
        s_press_rx = rx;
        s_press_ry = ry;
        s_press_ms = millis();
    }
    if (!pressed && s_last_pressed) {
        uint32_t dt = millis() - s_press_ms;
        if (dt >= 25 && s_press_rx >= 0 && s_press_ry >= 0) {
            if (s_mode == Mode::Test) {
                // Map raw -> screen with active calibration; light the
                // cell that screen-position falls in. This is a
                // diagnostic - no sample is captured for cal.
                int16_t sx = s_press_rx, sy = s_press_ry;
                ::touch_cal::apply(&sx, &sy);
                int idx = cell_for_screen(sx, sy);
                if (idx >= 0) {
                    bool fresh = (s_samples[idx].raw_x < 0);
                    s_samples[idx].raw_x = s_press_rx;
                    s_samples[idx].raw_y = s_press_ry;
                    paint_cell(idx);
                    if (fresh) {
                        s_captured++;
                        update_status();
                    }
                }
            } else {
                // CAL: assign tap to the active (highlighted) target.
                int idx = s_active;
                if (idx >= 0 && idx < CELLS) {
                    bool fresh = (s_samples[idx].raw_x < 0);
                    s_samples[idx].raw_x = s_press_rx;
                    s_samples[idx].raw_y = s_press_ry;
                    s_samples[idx].target_x = target_x_for_col(idx % N);
                    s_samples[idx].target_y = target_y_for_row(idx / N);
                    if (fresh) s_captured++;
                    int next = idx + 1;
                    if (next >= CELLS) next = 0;
                    set_active(next);
                    update_status();
                }
            }
        }
        s_press_rx = -1;
        s_press_ry = -1;
    }
    s_last_pressed = pressed;
}

static void make_grid(lv_obj_t *parent) {
    int cw = cell_w();
    int ch = cell_h();
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int idx = r * N + c;
            lv_obj_t *cell = lv_obj_create(parent);
            lv_obj_set_size(cell, cw - 2, ch - 2);
            lv_obj_set_pos(cell, c * cw + 1, r * ch + 1);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            s_cells[idx] = cell;
        }
    }
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000814), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    make_grid(s_root);
    repaint_all();

    // Status pill: top-center.
    s_lbl_status = lv_label_create(s_root);
    lv_label_set_text(s_lbl_status, "TEST   tap any cell");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xebf5ff), 0);
    lv_obj_set_style_bg_color(s_lbl_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lbl_status, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_lbl_status, 12, 0);
    lv_obj_set_style_pad_ver(s_lbl_status, 4, 0);
    lv_obj_set_style_radius(s_lbl_status, 6, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_CLICKABLE);

    // Left button: bottom-left. Test mode = START CAL, Cal mode = APPLY.
    s_btn_left = lv_button_create(s_root);
    lv_obj_set_size(s_btn_left, 160, 60);
    lv_obj_align(s_btn_left, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_radius(s_btn_left, 8, 0);
    lv_obj_add_event_cb(s_btn_left, on_left, LV_EVENT_CLICKED, NULL);
    s_btn_left_lbl = lv_label_create(s_btn_left);
    lv_obj_set_style_text_color(s_btn_left_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_btn_left_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_btn_left_lbl);

    // Right button: bottom-right. Test = CANCEL, Cal = BACK.
    s_btn_right = lv_button_create(s_root);
    lv_obj_set_size(s_btn_right, 160, 60);
    lv_obj_align(s_btn_right, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_radius(s_btn_right, 8, 0);
    lv_obj_add_event_cb(s_btn_right, on_right, LV_EVENT_CLICKED, NULL);
    s_btn_right_lbl = lv_label_create(s_btn_right);
    lv_obj_set_style_text_color(s_btn_right_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_btn_right_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_btn_right_lbl);

    update_status();
    return s_root;
}

void refresh() {
    static bool s_was_active = false;
    bool now_active = (lv_screen_active() == s_root);
    if (!s_timer) {
        s_timer = lv_timer_create(poll, 20, NULL);
    }
    if (now_active && !s_was_active) {
        // Default to Test mode on entry.
        s_mode = Mode::Test;
        clear_captures();
        s_last_pressed = false;
        update_status();
        lv_obj_invalidate(s_root);
    }
    s_was_active = now_active;
}

}  // namespace ui::touch_grid_screen
