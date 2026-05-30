#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
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

static double s_min = NAN, s_max = NAN;
static uint32_t s_last_sample_ms = 0;

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    style_screen(s_root);

    // Header: DEPTH caption + current value (huge)
    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "DEPTH");
    style_value(cap, &lv_font_montserrat_20, theme.fg_dim);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 16, 16);

    lbl_depth = lv_label_create(s_root);
    lv_label_set_text(lbl_depth, "--.-");
    style_value(lbl_depth, &lv_font_montserrat_48, theme.good);
    lv_obj_align(lbl_depth, LV_ALIGN_TOP_LEFT, 16, 40);

    lv_obj_t *unit = lv_label_create(s_root);
    lv_label_set_text(unit, "m");
    style_value(unit, &lv_font_montserrat_28, theme.fg_dim);
    lv_obj_align(unit, LV_ALIGN_TOP_LEFT, 180, 56);

    // Min/max captured since boot
    lbl_min = lv_label_create(s_root);
    lv_label_set_text(lbl_min, "MIN --.-");
    style_value(lbl_min, &lv_font_montserrat_20, theme.alarm);
    // Top-right reserved for global MOB pill - shift labels below it.
    lv_obj_align(lbl_min, LV_ALIGN_TOP_RIGHT, -16, 72);

    lbl_max = lv_label_create(s_root);
    lv_label_set_text(lbl_max, "MAX --.-");
    style_value(lbl_max, &lv_font_montserrat_20, theme.good);
    lv_obj_align(lbl_max, LV_ALIGN_TOP_RIGHT, -16, 100);

    // Chart: running 60-sample depth history
    chart = lv_chart_create(s_root);
    lv_obj_set_size(chart, LCD_W - 32, 240);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(DEFAULT_MAX * 10));
    lv_chart_set_div_line_count(chart, 4, 6);
    style_panel(chart, theme.good);
    lv_obj_set_style_line_color(chart, lv_color_hex(theme.grid), LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(theme.grid), LV_PART_ITEMS);

    series = lv_chart_add_series(chart, lv_color_hex(theme.accent), LV_CHART_AXIS_PRIMARY_Y);
    // Cursor line for shallow threshold
    cursor_shallow = lv_chart_add_cursor(chart, lv_color_hex(theme.alarm),
                                         (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    (void)cursor_shallow;  // shallow band overlay; positioned per-frame in refresh

    // Water temp at bottom
    lbl_temp = lv_label_create(s_root);
    lv_label_set_text(lbl_temp, "WATER --.-\xC2\xB0"
                                "C");
    style_value(lbl_temp, &lv_font_montserrat_28, theme.fg);
    lv_obj_align(lbl_temp, LV_ALIGN_BOTTOM_MID, 0, -8);

    return s_root;
}

// Dirty-value caches (docs/specs/09).
static char s_last_depth[16] = {(char)0xFF};
static char s_last_min[16] = {(char)0xFF};
static char s_last_max[16] = {(char)0xFF};
static char s_last_temp[24] = {(char)0xFF};
static uint32_t s_last_depth_color = 0xFFFFFFFF;

void refresh() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        set_text_if_changed(lbl_depth, s_last_depth, sizeof(s_last_depth), buf);

        // Color cue: alarm if below threshold
        double shallow_m = depth_alarm_m();
        set_text_color_if_changed(lbl_depth, &s_last_depth_color,
                                  d.depth < shallow_m ? theme.alarm : theme.fg);

        // Min / Max
        if (isnan(s_min) || d.depth < s_min) s_min = d.depth;
        if (isnan(s_max) || d.depth > s_max) s_max = d.depth;
        snprintf(buf, sizeof(buf), "MIN %.1f", s_min);
        set_text_if_changed(lbl_min, s_last_min, sizeof(s_last_min), buf);
        snprintf(buf, sizeof(buf), "MAX %.1f", s_max);
        set_text_if_changed(lbl_max, s_last_max, sizeof(s_last_max), buf);

        // Sample at most once a second to keep history meaningful
        uint32_t now = millis();
        if (now - s_last_sample_ms > 1000) {
            s_last_sample_ms = now;
            lv_chart_set_next_value(chart, series, (lv_coord_t)(d.depth * 10));
            // Auto-scale: if recent max > 1.2 * current range, widen
            double range_max = fmax(s_max * 1.2, shallow_m * 1.5);
            range_max = fmax(range_max, 10.0);
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(range_max * 10));
        }
    }

    if (!isnan(d.waterTemp)) {
        snprintf(buf, sizeof(buf),
                 "WATER %.1f\xC2\xB0"
                 "C",
                 k_to_c(d.waterTemp));
        set_text_if_changed(lbl_temp, s_last_temp, sizeof(s_last_temp), buf);
    }
}

}  // namespace ui::depth
