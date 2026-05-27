#include "screens.h"
#include "ui_theme.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "net.h"
#include "touch_cal.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

// 10x10 touch coverage / calibration grid. Doubles as a display test
// pattern: every cell is a tap target, cells light up as captured, and
// the user can see which areas don't respond. When enough cells are
// captured (>= MIN_FOR_FIT) APPLY fits a 6-parameter affine to all the
// samples (much more robust than the 4-point screen at /touch_cal).
//
// Reachable via console: `touch-grid` (also `grid-cal`).
//
// Raw GT911 samples come from main.cpp's main_touch_raw() snapshot so
// calibration sees pre-transform coordinates regardless of any matrix
// already in effect.

extern "C" {
void main_touch_raw(int *raw_x, int *raw_y, int *pressed);
}

namespace ui::touch_grid_screen {

static constexpr int N = 10;
static constexpr int CELLS = N * N;
static constexpr int MIN_FOR_FIT = 12;  // need enough samples for a stable affine

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_cells[CELLS] = {nullptr};
static lv_obj_t *s_lbl_status = nullptr;
static lv_obj_t *s_btn_apply = nullptr;
static lv_obj_t *s_btn_apply_lbl = nullptr;
static lv_obj_t *s_btn_cancel = nullptr;
static lv_timer_t *s_timer = nullptr;

// Stored sample per cell (raw GT911 coords). raw_x == -1 means
// "not yet captured".
static ::touch_cal::Sample s_samples[CELLS];
static int s_captured = 0;

// Active target cell index. Without a known-good calibration we can't
// guess which cell the user meant to tap from raw coords; instead we
// walk a single highlighted target through the grid (row-major). The
// user taps wherever they see the bright cell; we record raw coords
// against that target's screen position. After completing the walk
// (or hitting APPLY mid-way) we solve the affine.
static int s_active = 0;

// Edge-detect touch state across poll() invocations.
static bool s_last_pressed = false;
static int16_t s_press_rx = -1, s_press_ry = -1;
static uint32_t s_press_ms = 0;

static int cell_w() { return LCD_W / N; }
static int cell_h() { return LCD_H / N; }
static int target_x_for_col(int c) { return c * cell_w() + cell_w() / 2; }
static int target_y_for_row(int r) { return r * cell_h() + cell_h() / 2; }

static void update_status() {
    if (!s_lbl_status) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "GRID %d / %d   tap to fill", s_captured, CELLS);
    lv_label_set_text(s_lbl_status, buf);
    if (s_btn_apply && s_btn_apply_lbl) {
        if (s_captured >= MIN_FOR_FIT) {
            lv_obj_set_style_bg_color(s_btn_apply, lv_color_hex(0x1b6e3a), 0);
            lv_label_set_text(s_btn_apply_lbl, "APPLY");
        } else {
            lv_obj_set_style_bg_color(s_btn_apply, lv_color_hex(0x3b4250), 0);
            char b2[32];
            snprintf(b2, sizeof(b2), "%d more", MIN_FOR_FIT - s_captured);
            lv_label_set_text(s_btn_apply_lbl, b2);
        }
    }
}

static void mark_cell_filled(int idx) {
    if (idx < 0 || idx >= CELLS) return;
    if (!s_cells[idx]) return;
    lv_obj_set_style_bg_color(s_cells[idx], lv_color_hex(0x1b6e3a), 0);
    lv_obj_set_style_bg_opa(s_cells[idx], LV_OPA_COVER, 0);
}

static void mark_cell_active(int idx, bool active) {
    if (idx < 0 || idx >= CELLS) return;
    if (!s_cells[idx]) return;
    if (active) {
        lv_obj_set_style_bg_color(s_cells[idx], lv_color_hex(0xf6a21a), 0);
        lv_obj_set_style_border_color(s_cells[idx], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(s_cells[idx], 2, 0);
    } else if (s_samples[idx].raw_x >= 0) {
        // already captured - keep filled green
        mark_cell_filled(idx);
        lv_obj_set_style_border_color(s_cells[idx], lv_color_hex(0x344050), 0);
        lv_obj_set_style_border_width(s_cells[idx], 1, 0);
    } else {
        lv_obj_set_style_bg_color(s_cells[idx], lv_color_hex(0x1c2632), 0);
        lv_obj_set_style_border_color(s_cells[idx], lv_color_hex(0x344050), 0);
        lv_obj_set_style_border_width(s_cells[idx], 1, 0);
    }
}

static void set_active(int idx) {
    int prev = s_active;
    s_active = idx;
    if (prev != idx) {
        mark_cell_active(prev, false);
        mark_cell_active(idx, true);
    }
}

static void apply_and_save() {
    // Build a tight array of captured samples to feed solve().
    ::touch_cal::Sample fit[CELLS];
    int n = 0;
    for (int i = 0; i < CELLS; ++i) {
        if (s_samples[i].raw_x >= 0) fit[n++] = s_samples[i];
    }
    if (n < MIN_FOR_FIT) return;
    ::touch_cal::Matrix m;
    if (!::touch_cal::solve(fit, n, m)) {
        if (s_lbl_status)
            lv_label_set_text(s_lbl_status, "Solve failed - too sparse or noisy");
        return;
    }
    ::touch_cal::set(m);
    net::logf("[cal-grid] applied affine from %d samples: "
              "a=%.4f b=%.4f c=%.2f d=%.4f e=%.4f f=%.2f",
              n, m.a, m.b, m.c, m.d, m.e, m.f);
    ui::show_by_id("settings");
}

static void on_apply(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    apply_and_save();
}

static void on_cancel(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui::show_by_id("settings");
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
        if (dt >= 60 && s_press_rx >= 0 && s_press_ry >= 0 &&
            s_active >= 0 && s_active < CELLS) {
            // Always assign the tap to the currently highlighted target -
            // that's the user's intent. We can't trust raw coords to
            // identify the cell because the panel's offset/skew is
            // precisely what we're trying to measure.
            int idx = s_active;
            bool fresh = (s_samples[idx].raw_x < 0);
            s_samples[idx].raw_x = s_press_rx;
            s_samples[idx].raw_y = s_press_ry;
            s_samples[idx].target_x = target_x_for_col(idx % N);
            s_samples[idx].target_y = target_y_for_row(idx / N);
            if (fresh) s_captured++;

            int next = idx + 1;
            if (next >= CELLS) next = 0;  // wrap so user can refine
            set_active(next);
            update_status();
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
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x1c2632), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(0x344050), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            // Not clickable - we read taps via the raw poll, not LVGL
            // hit-testing (which can't be trusted before we calibrate).
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

    // Floating status pill at top-center.
    s_lbl_status = lv_label_create(s_root);
    lv_label_set_text(s_lbl_status, "GRID 0 / 100   tap to fill");
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xebf5ff), 0);
    lv_obj_set_style_bg_color(s_lbl_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lbl_status, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_lbl_status, 12, 0);
    lv_obj_set_style_pad_ver(s_lbl_status, 4, 0);
    lv_obj_set_style_radius(s_lbl_status, 6, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_CLICKABLE);

    // APPLY button bottom-right.
    s_btn_apply = lv_button_create(s_root);
    lv_obj_set_size(s_btn_apply, 160, 60);
    lv_obj_align(s_btn_apply, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(s_btn_apply, lv_color_hex(0x3b4250), 0);
    lv_obj_set_style_radius(s_btn_apply, 8, 0);
    lv_obj_add_event_cb(s_btn_apply, on_apply, LV_EVENT_CLICKED, NULL);
    s_btn_apply_lbl = lv_label_create(s_btn_apply);
    lv_label_set_text(s_btn_apply_lbl, "12 more");
    lv_obj_set_style_text_color(s_btn_apply_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_btn_apply_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_btn_apply_lbl);

    // CANCEL button bottom-left.
    s_btn_cancel = lv_button_create(s_root);
    lv_obj_set_size(s_btn_cancel, 160, 60);
    lv_obj_align(s_btn_cancel, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(s_btn_cancel, lv_color_hex(0x803030), 0);
    lv_obj_set_style_radius(s_btn_cancel, 8, 0);
    lv_obj_add_event_cb(s_btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_btn_cancel);
    lv_label_set_text(lbl, "CANCEL");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    return s_root;
}

void refresh() {
    if (!s_timer) {
        s_timer = lv_timer_create(poll, 50, NULL);
    }
    if (lv_screen_active() == s_root) {
        // If re-entered after a previous run, reset all captures and
        // start the walking target at cell 0.
        s_captured = 0;
        for (int i = 0; i < CELLS; ++i) {
            s_samples[i].raw_x = -1;
            s_samples[i].raw_y = -1;
            if (s_cells[i]) {
                lv_obj_set_style_bg_color(s_cells[i], lv_color_hex(0x1c2632), 0);
                lv_obj_set_style_border_color(s_cells[i], lv_color_hex(0x344050), 0);
                lv_obj_set_style_border_width(s_cells[i], 1, 0);
            }
        }
        s_last_pressed = false;
        s_active = -1;
        set_active(0);
        update_status();
        lv_obj_invalidate(s_root);
    }
}

}  // namespace ui::touch_grid_screen
