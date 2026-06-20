#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"
#include "config_runtime.h"
#include "value_format.h"

#include <math.h>
#include <stdio.h>

// Depth screen, hand-built to mirror the Trip screen's layout (title + a hero
// card spanning the top half + a 3-stat row below) instead of the HeroPlus
// template. The hero value uses font_xl_64 (bigger than Trip's montserrat_48)
// because depth-below-keel is the headline datum on this screen.
//
//   HERO  = depth below keel (environment.depth.belowKeel, "m")
//   stat1 = water temperature (environment.water.temperature, "C")
//   stat2 = speed over ground (navigation.speedOverGround, "kn")
//   stat3 = true wind angle   (environment.wind.angleTrueWater, port/stbd)
//
// The s_spec + collect_paths below are retained purely so the per-screen
// subscription manager still requests those four paths (the bindings are the
// single source of truth for what this screen needs the server to send); the
// hand-built build()/refresh() read sk::Data directly and do not call the
// HeroPlus template renderer.

namespace ui::depth {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *lbl_depth, *lbl_temp, *lbl_sog, *lbl_twa;

// Depth-history sparkline (bottom card). The ring buffer must be a module
// static (not a stack local) per the firmware memory traps; lv_coord_t[60] is
// ~240 B and lives in .bss. s_hist_n is unused for chart bookkeeping but kept
// to track whether we have any samples (for the initial Y autoscale).
static lv_obj_t *s_chart = nullptr;
static lv_chart_series_t *s_series = nullptr;
static const int HIST_N = 60;
static lv_coord_t s_hist[HIST_N];
static int s_hist_n = 0;
static uint32_t s_hist_last_ms = 0;

static const ui::layouts::MetricBinding s_tiles[] = {
    {"depthKeel",
     "BELOW KEEL",
     "m",
     ui::layouts::MetricSource::DepthKeel_m,
     0x57c7d8 /*accent*/,
     nullptr,
     3,
     {
         {"TEMP", ui::layouts::MetricSource::WaterTemp_C},
         {"SOG", ui::layouts::MetricSource::SOG_kn},
         {"TWA", ui::layouts::MetricSource::TWA_deg},
     },
     ui::layouts::WidgetKind::Numeric},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "depth",
    "Depth",
    ui::layouts::TemplateId::HeroPlus,
    s_tiles,
    sizeof(s_tiles) / sizeof(s_tiles[0]),
    0,
};

static void collect_paths(sk::SubscriptionSet &out) {
    ui::layouts::collect_paths(s_spec, out);
}

// Trip-style stat card: panel + caption (montserrat_14, dim) over a value
// (montserrat_28). Mirrors ui::trip::make_stat.
static lv_obj_t *make_stat(lv_obj_t *parent, const char *cap, int x, int y, int w, int h,
                           lv_obj_t **value_out, uint32_t color) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *ttl = lv_label_create(c);
    lv_label_set_text(ttl, cap);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(c);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(color), 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    *value_out = val;
    return c;
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
    lv_label_set_text(title, "DEPTH");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Hero depth card. Shorter than Trip's 200 px hero to make room for the
    // doubled (~128 px) value and the bottom history strip.
    lv_obj_t *hero = lv_obj_create(s_root);
    lv_obj_set_size(hero, LCD_W - 16, 160);
    lv_obj_set_pos(hero, 8, 38);
    lv_obj_set_style_bg_color(hero, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_pad_all(hero, 8, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hero, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *cap = lv_label_create(hero);
    lv_label_set_text(cap, "BELOW KEEL");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    // Hero value: the 64 px font is doubled to ~128 px via an LVGL transform
    // scale (there is no 128 px bitmap font; 256 = 1.0, so 512 = 2.0). The
    // pivot is the label's own center so it scales in place. Centered with a
    // slight left bias to leave room for the "m" unit on the right.
    lbl_depth = lv_label_create(hero);
    lv_label_set_text(lbl_depth, "--");
    lv_obj_set_style_text_font(lbl_depth, &font_xl_64, 0);
    lv_obj_set_style_text_color(lbl_depth, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_transform_scale(lbl_depth, 512, 0);
    lv_obj_set_style_transform_pivot_x(lbl_depth, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(lbl_depth, LV_PCT(50), 0);
    lv_obj_align(lbl_depth, LV_ALIGN_CENTER, -24, 8);

    lv_obj_t *unit = lv_label_create(hero);
    lv_label_set_text(unit, "m");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_RIGHT, -12, -8);

    // Three stats side by side. Shorter (88 px) and higher than Trip's row to
    // leave a ~170 px history strip at the bottom.
    int row_y = 204;
    int col_w = (LCD_W - 32) / 3;
    make_stat(s_root, "WATER TEMP", 8, row_y, col_w, 88, &lbl_temp, theme.fg);
    make_stat(s_root, "SOG", 8 + col_w + 8, row_y, col_w, 88, &lbl_sog, theme.fg);
    make_stat(s_root, "TWA", 8 + (col_w + 8) * 2, row_y, col_w - 8, 88, &lbl_twa, theme.good);

    // Depth-history card (sparkline) spans the bottom (y 298..468).
    lv_obj_t *hist = lv_obj_create(s_root);
    lv_obj_set_size(hist, LCD_W - 16, 170);
    lv_obj_set_pos(hist, 8, 298);
    lv_obj_set_style_bg_color(hist, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(hist, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(hist, 1, 0);
    lv_obj_set_style_radius(hist, 8, 0);
    lv_obj_set_style_pad_all(hist, 8, 0);
    lv_obj_clear_flag(hist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hist, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *hist_cap = lv_label_create(hist);
    lv_label_set_text(hist_cap, "DEPTH HISTORY");
    lv_obj_set_style_text_font(hist_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hist_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(hist_cap, LV_ALIGN_TOP_LEFT, 0, 0);

    s_chart = lv_chart_create(hist);
    lv_obj_set_size(s_chart, LCD_W - 16 - 16 - 8, 170 - 16 - 22);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, HIST_N);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    // No round point markers, just the line.
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(theme.grid), LV_PART_MAIN);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(theme.accent), LV_CHART_AXIS_PRIMARY_Y);

    ui::set_screen_collect_paths(s_spec.screen_id, collect_paths);
    return s_root;
}

// Dirty-value caches (docs/specs/09).
static char s_last_depth[16] = {(char)0xFF};
static char s_last_temp[16] = {(char)0xFF};
static char s_last_sog[16] = {(char)0xFF};
static char s_last_twa[16] = {(char)0xFF};

void refresh() {
    if (!s_root) return;
    sk::Data d;
    sk::copyData(d);
    char buf[32];

    // Hero: depth below keel, k/M-scaled per the depth unit format.
    vfmt::format_scaled(d.depthKeel, config::format().depth, buf, sizeof(buf));
    set_text_if_changed(lbl_depth, s_last_depth, sizeof(s_last_depth), buf);

    // History sparkline: push depth-below-keel (in cm to keep integer
    // resolution) once per second, then autoscale Y from the ring buffer.
    if (s_chart && s_series && !isnan(d.depthKeel)) {
        uint32_t now = millis();
        if (s_hist_last_ms == 0 || now - s_hist_last_ms >= 1000) {
            s_hist_last_ms = now;
            lv_coord_t v = (lv_coord_t)(d.depthKeel * 100.0);  // meters -> cm
            s_hist[s_hist_n % HIST_N] = v;
            if (s_hist_n < HIST_N) s_hist_n++;
            lv_chart_set_next_value(s_chart, s_series, v);

            lv_coord_t lo = s_hist[0], hi = s_hist[0];
            for (int i = 1; i < s_hist_n; i++) {
                if (s_hist[i] < lo) lo = s_hist[i];
                if (s_hist[i] > hi) hi = s_hist[i];
            }
            if (hi - lo < 50) hi = lo + 50;  // floor span at 0.5 m
            lv_coord_t pad = (hi - lo) / 10 + 5;
            lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo - pad, hi + pad);
        }
    }

    // Water temp (K -> C), with a trailing degree/C since the caption is
    // "WATER TEMP" (no unit in caption).
    if (isnan(d.waterTemp)) {
        set_text_if_changed(lbl_temp, s_last_temp, sizeof(s_last_temp), "--");
    } else {
        char tbuf[24];
        vfmt::format_scaled(k_to_c(d.waterTemp), config::format().temperature, tbuf, sizeof(tbuf));
        snprintf(buf, sizeof(buf), "%s\xC2\xB0", tbuf);  // append degree sign
        set_text_if_changed(lbl_temp, s_last_temp, sizeof(s_last_temp), buf);
    }

    // SOG in knots, 1 decimal.
    if (isnan(d.sog))
        snprintf(buf, sizeof(buf), "--");
    else
        snprintf(buf, sizeof(buf), "%.1f kn", mps_to_kn(d.sog));
    set_text_if_changed(lbl_sog, s_last_sog, sizeof(s_last_sog), buf);

    // TWA in degrees with port/stbd suffix (mirror ui_layouts::format_metric).
    if (isnan(d.twa)) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        double deg = rad_to_deg_pos(d.twa);
        bool stbd = deg <= 180.0;
        snprintf(buf, sizeof(buf), "%.0f%c", stbd ? deg : 360 - deg, stbd ? 'S' : 'P');
    }
    set_text_if_changed(lbl_twa, s_last_twa, sizeof(s_last_twa), buf);
}

}  // namespace ui::depth
