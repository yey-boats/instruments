#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "board_pins.h"

#include <math.h>
#include <stdio.h>

// Fullscreen depth page: huge current depth + 60-sample running chart with
// shallow-water alarm band shaded red.

namespace ui::depth {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_depth = nullptr;
static lv_obj_t *lbl_temp = nullptr;
static lv_obj_t *lbl_min = nullptr;
static lv_obj_t *lbl_max = nullptr;
static lv_obj_t *chart = nullptr;
static lv_chart_series_t *series = nullptr;
static lv_chart_cursor_t *cursor_shallow = nullptr;

static const int CHART_POINTS = 60;
static const double DEFAULT_MAX = 30.0;
static const double SHALLOW_M = 3.0;

static double s_min = NAN, s_max = NAN;
static uint32_t s_last_sample_ms = 0;

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

    // Header: DEPTH caption + current value (huge)
    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "DEPTH");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 16, 16);

    lbl_depth = lv_label_create(s_root);
    lv_label_set_text(lbl_depth, "--.-");
    lv_obj_set_style_text_font(lbl_depth, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_depth, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_depth, LV_ALIGN_TOP_LEFT, 16, 40);

    lv_obj_t *unit = lv_label_create(s_root);
    lv_label_set_text(unit, "m");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(unit, LV_ALIGN_TOP_LEFT, 180, 56);

    // Min/max captured since boot
    lbl_min = lv_label_create(s_root);
    lv_label_set_text(lbl_min, "MIN --.-");
    lv_obj_set_style_text_font(lbl_min, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_min, lv_color_hex(theme.alarm), 0);
    lv_obj_align(lbl_min, LV_ALIGN_TOP_RIGHT, -16, 16);

    lbl_max = lv_label_create(s_root);
    lv_label_set_text(lbl_max, "MAX --.-");
    lv_obj_set_style_text_font(lbl_max, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_max, lv_color_hex(theme.good), 0);
    lv_obj_align(lbl_max, LV_ALIGN_TOP_RIGHT, -16, 44);

    // Chart: running 60-sample depth history
    chart = lv_chart_create(s_root);
    lv_obj_set_size(chart, LCD_W - 32, 240);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(DEFAULT_MAX * 10));
    lv_chart_set_div_line_count(chart, 4, 6);
    lv_obj_set_style_bg_color(chart, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(theme.grid), LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(theme.grid), LV_PART_ITEMS);

    series = lv_chart_add_series(chart, lv_color_hex(theme.accent), LV_CHART_AXIS_PRIMARY_Y);
    // Cursor line for shallow threshold
    cursor_shallow = lv_chart_add_cursor(chart, lv_color_hex(theme.alarm),
                                         (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    (void)cursor_shallow;  // shallow band overlay; positioned per-frame in refresh

    // Water temp at bottom
    lbl_temp = lv_label_create(s_root);
    lv_label_set_text(lbl_temp, "WATER --.-\xC2\xB0""C");
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(theme.fg), 0);
    lv_obj_align(lbl_temp, LV_ALIGN_BOTTOM_MID, 0, -8);

    return s_root;
}

void refresh() {
    const sk::Data &d = sk::data;
    char buf[64];

    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        lv_label_set_text(lbl_depth, buf);

        // Color cue: alarm if below threshold
        lv_obj_set_style_text_color(
            lbl_depth, lv_color_hex(d.depth < SHALLOW_M ? theme.alarm : theme.fg), 0);

        // Min / Max
        if (isnan(s_min) || d.depth < s_min) s_min = d.depth;
        if (isnan(s_max) || d.depth > s_max) s_max = d.depth;
        snprintf(buf, sizeof(buf), "MIN %.1f", s_min);
        lv_label_set_text(lbl_min, buf);
        snprintf(buf, sizeof(buf), "MAX %.1f", s_max);
        lv_label_set_text(lbl_max, buf);

        // Sample at most once a second to keep history meaningful
        uint32_t now = millis();
        if (now - s_last_sample_ms > 1000) {
            s_last_sample_ms = now;
            lv_chart_set_next_value(chart, series, (lv_coord_t)(d.depth * 10));
            // Auto-scale: if recent max > 1.2 * current range, widen
            double range_max = fmax(s_max * 1.2, SHALLOW_M * 1.5);
            range_max = fmax(range_max, 10.0);
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(range_max * 10));
        }
    }

    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf), "WATER %.1f\xC2\xB0""C", k_to_c(d.waterTemp));
        lv_label_set_text(lbl_temp, buf);
    }
}

}  // namespace ui::depth
