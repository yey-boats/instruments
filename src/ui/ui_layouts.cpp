#include "ui_layouts.h"

#include "ui_theme.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "app_events.h"
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui::layouts {

// ---------------------------------------------------------------------------
// Per-template instance state. We attach this as user_data on the
// template's root object so update() can find its labels/widgets
// without depending on file-scope statics (templates can be
// instantiated multiple times, e.g. test screens).

struct QuadGridTile {
    lv_obj_t *root;
    lv_obj_t *cap;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *secondary;
    lv_obj_t *extras[4];      // multi-row tiles - small label+value lines
    char last_value[24];
    char last_secondary[24];
    char last_extras[4][32];
    int idx;  // metric index into spec.metrics[]
};

struct QuadGridState {
    QuadGridTile tiles[4];
};

// ---------------------------------------------------------------------------
// Metric formatting. Returns the primary value text, and optionally
// fills `secondary` with a short context string. NaN -> "--".

static void format_metric(const MetricBinding &m, const sk::Data &d,
                          char *primary, size_t pcap,
                          char *secondary, size_t scap) {
    secondary[0] = 0;
    switch (m.source) {
    case MetricSource::AWS_kn:
        if (!isnan(d.aws)) snprintf(primary, pcap, "%.1f", mps_to_kn(d.aws));
        else snprintf(primary, pcap, "--");
        if (!isnan(d.awa)) {
            double deg = rad_to_deg_pos(d.awa);
            bool stbd = deg <= 180.0;
            snprintf(secondary, scap, "%.0f%c", stbd ? deg : 360 - deg,
                     stbd ? 'S' : 'P');
        }
        break;
    case MetricSource::TWS_kn:
        if (!isnan(d.tws)) snprintf(primary, pcap, "%.1f", mps_to_kn(d.tws));
        else snprintf(primary, pcap, "--");
        if (!isnan(d.twa)) {
            double deg = rad_to_deg_pos(d.twa);
            bool stbd = deg <= 180.0;
            snprintf(secondary, scap, "%.0f%c", stbd ? deg : 360 - deg,
                     stbd ? 'S' : 'P');
        }
        break;
    case MetricSource::AWA_deg:
        if (!isnan(d.awa)) {
            double deg = rad_to_deg_pos(d.awa);
            bool stbd = deg <= 180.0;
            snprintf(primary, pcap, "%.0f%c", stbd ? deg : 360 - deg,
                     stbd ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::TWA_deg:
        if (!isnan(d.twa)) {
            double deg = rad_to_deg_pos(d.twa);
            bool stbd = deg <= 180.0;
            snprintf(primary, pcap, "%.0f%c", stbd ? deg : 360 - deg,
                     stbd ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::SOG_kn:
        if (!isnan(d.sog)) snprintf(primary, pcap, "%.1f", mps_to_kn(d.sog));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::COG_deg:
        if (!isnan(d.cogTrue)) snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.cogTrue));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::HDG_deg:
        if (!isnan(d.headingTrue)) snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.headingTrue));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::Depth_m:
        if (!isnan(d.depth)) snprintf(primary, pcap, "%.1f", d.depth);
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::WaterTemp_C:
        if (!isnan(d.waterTemp)) snprintf(primary, pcap, "%.1f", k_to_c(d.waterTemp));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::BatteryV:
        if (!isnan(d.battVoltage)) snprintf(primary, pcap, "%.2f", d.battVoltage);
        else snprintf(primary, pcap, "--");
        if (!isnan(d.battSoc)) {
            snprintf(secondary, scap, "%.0f%%", d.battSoc * 100.0);
        }
        break;
    case MetricSource::BatterySOC_pct:
        if (!isnan(d.battSoc)) snprintf(primary, pcap, "%.0f%%", d.battSoc * 100.0);
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::DTW:
        if (!isnan(d.dtw)) {
            if (d.dtw >= 1852.0) snprintf(primary, pcap, "%.2f", d.dtw / 1852.0);
            else snprintf(primary, pcap, "%.0f", d.dtw);
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::BTW_deg:
        if (!isnan(d.btw)) snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.btw));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::XTE:
        if (!isnan(d.xte)) snprintf(primary, pcap, "%.0f", fabs(d.xte));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::VMG_kn:
        if (!isnan(d.vmg)) snprintf(primary, pcap, "%.1f", mps_to_kn(d.vmg));
        else snprintf(primary, pcap, "--");
        break;
    case MetricSource::Position:
        if (!isnan(d.lat) && !isnan(d.lon)) {
            format_position(d.lat, d.lon, pos_format(), primary, pcap);
        } else {
            snprintf(primary, pcap, "no fix");
        }
        break;
    case MetricSource::APState:
        if (d.apState[0]) snprintf(primary, pcap, "%s", d.apState);
        else snprintf(primary, pcap, "off");
        break;
    case MetricSource::None:
    default:
        snprintf(primary, pcap, "--");
        break;
    }
}

// ---------------------------------------------------------------------------
// quad_grid template - 2x2 tile dashboard
//
// Layout: 4 tiles laid out in a 2x2 grid, each ~240x230 px on a 480x480
// panel. Top-of-screen has the MOB-safe band reserved (see spec 09
// safe zone); tiles start at y=72 and the bottom row reaches y=472.

static constexpr int QG_TOP_Y = 72;            // clear MOB pill
static constexpr int QG_BOTTOM_Y = LCD_H - 8;  // small bottom margin
static constexpr int QG_TILE_W = (LCD_W - 12) / 2;
static constexpr int QG_TILE_H = (QG_BOTTOM_Y - QG_TOP_Y - 4) / 2;
static constexpr int QG_GAP = 4;

static void tile_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto *target = (const char *)lv_event_get_user_data(e);
    net::logf("[layout] tile CLICKED target=%s", target ? target : "(null)");
    if (!target || !*target) return;
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, target, sizeof(c.a) - 1);
    c.t_post_us = micros();
    app::post(c, 0);
}

static QuadGridTile build_tile(lv_obj_t *parent, int x, int y, int w, int h,
                                const MetricBinding &m) {
    QuadGridTile t = {};
    t.idx = -1;
    strncpy(t.last_value, "\xFF", sizeof(t.last_value));
    strncpy(t.last_secondary, "\xFF", sizeof(t.last_secondary));
    for (int i = 0; i < 4; ++i) strncpy(t.last_extras[i], "\xFF", sizeof(t.last_extras[0]));

    t.root = lv_obj_create(parent);
    lv_obj_set_size(t.root, w, h);
    lv_obj_set_pos(t.root, x, y);
    lv_obj_set_style_bg_color(t.root, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(t.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t.root, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(t.root, 1, 0);
    lv_obj_set_style_radius(t.root, 8, 0);
    lv_obj_set_style_pad_all(t.root, 10, 0);
    lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLLABLE);

    // Accent rail.
    lv_obj_t *rail = lv_obj_create(t.root);
    lv_obj_set_size(rail, 4, h - 20);
    lv_obj_set_pos(rail, 0, 0);
    lv_obj_set_style_bg_color(rail, lv_color_hex(m.accent), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 2, 0);
    lv_obj_set_style_pad_all(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);

    // Caption.
    t.cap = lv_label_create(t.root);
    lv_label_set_text(t.cap, m.label ? m.label : "");
    lv_obj_set_style_text_font(t.cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t.cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(t.cap, 12, 4);
    lv_obj_clear_flag(t.cap, LV_OBJ_FLAG_CLICKABLE);

    bool has_extras = (m.extras_count > 0);

    // Primary value. If extras are present, shrink + pin to upper area
    // so the extras have room below. Otherwise it stays large + centered.
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    const lv_font_t *primary_font = has_extras ? &lv_font_montserrat_28
                                                : &lv_font_montserrat_48;
    lv_obj_set_style_text_font(t.value, primary_font, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.fg), 0);
    if (has_extras) {
        lv_obj_align(t.value, LV_ALIGN_TOP_LEFT, 12, 26);
    } else {
        lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // Unit.
    if (m.unit && m.unit[0]) {
        t.unit = lv_label_create(t.root);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        if (has_extras) {
            lv_obj_align(t.unit, LV_ALIGN_TOP_LEFT, 12 + 90, 32);
        } else {
            lv_obj_align(t.unit, LV_ALIGN_CENTER, 0, 30);
        }
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }

    if (!has_extras) {
        // Classic Hero layout: secondary in bottom-right.
        t.secondary = lv_label_create(t.root);
        lv_label_set_text(t.secondary, "");
        lv_obj_set_style_text_font(t.secondary, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(t.secondary, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
        lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // Multi-row layout: stack up to 4 extras below the primary value.
        for (uint8_t i = 0; i < m.extras_count && i < 4; ++i) {
            t.extras[i] = lv_label_create(t.root);
            lv_label_set_text(t.extras[i], "");
            lv_obj_set_style_text_font(t.extras[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(t.extras[i], lv_color_hex(theme.fg), 0);
            // Row positions packed below the primary value area.
            lv_obj_align(t.extras[i], LV_ALIGN_TOP_LEFT, 12, 76 + i * 22);
            lv_obj_clear_flag(t.extras[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }

    if (m.target_screen && m.target_screen[0]) {
        lv_obj_add_event_cb(t.root, tile_clicked_cb, LV_EVENT_CLICKED,
                            (void *)m.target_screen);
    } else {
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_CLICKABLE);
    }
    return t;
}

static lv_obj_t *create_quad_grid(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    QuadGridState *st = (QuadGridState *)heap_caps_calloc(1, sizeof(QuadGridState),
                                                          MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] quad_grid alloc failed");
        return root;  // empty screen, caller still gets a valid handle
    }

    // 2x2 layout; missing metrics leave the tile slot blank.
    static const MetricBinding empty_metric = {"", "", "", MetricSource::None, 0x222222, nullptr};
    for (int i = 0; i < 4; ++i) {
        const MetricBinding &m = (i < spec.metric_count) ? spec.metrics[i] : empty_metric;
        int col = i % 2;
        int row = i / 2;
        int x = QG_GAP + col * (QG_TILE_W + QG_GAP);
        int y = QG_TOP_Y + row * (QG_TILE_H + QG_GAP);
        st->tiles[i] = build_tile(root, x, y, QG_TILE_W, QG_TILE_H, m);
        st->tiles[i].idx = (i < spec.metric_count) ? i : -1;
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_quad_grid(lv_obj_t *root, const ScreenVariantSpec &spec,
                              const sk::Data &data) {
    if (!root) return;
    auto *st = (QuadGridState *)lv_obj_get_user_data(root);
    if (!st) return;
    for (int i = 0; i < 4; ++i) {
        QuadGridTile &t = st->tiles[i];
        if (t.idx < 0 || t.idx >= spec.metric_count) continue;
        const MetricBinding &m = spec.metrics[t.idx];

        char pri[24], sec[24];
        format_metric(m, data, pri, sizeof(pri), sec, sizeof(sec));
        ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);

        if (t.secondary) {
            ui::set_text_if_changed(t.secondary, t.last_secondary,
                                    sizeof(t.last_secondary), sec);
        }
        // Render extras (multi-row tiles). Each extra reuses
        // format_metric on a synthetic MetricBinding so all the
        // unit/format logic is shared.
        for (uint8_t e = 0; e < m.extras_count && e < 4; ++e) {
            if (!t.extras[e]) continue;
            MetricBinding eb = {};
            eb.source = m.extras[e].source;
            char ep[24], esec[24];
            format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
            char row[32];
            if (m.extras[e].label && m.extras[e].label[0]) {
                snprintf(row, sizeof(row), "%s %s", m.extras[e].label, ep);
            } else {
                snprintf(row, sizeof(row), "%s", ep);
            }
            ui::set_text_if_changed(t.extras[e], t.last_extras[e],
                                    sizeof(t.last_extras[e]), row);
        }
    }
}

// ---------------------------------------------------------------------------
// hero_plus template - one huge primary value with up to 4 secondary
// stats stacked beneath it. Used by single-purpose screens (depth, wind
// detail, single-battery view).

struct HeroPlusState {
    lv_obj_t *primary_value;
    lv_obj_t *primary_unit;
    lv_obj_t *primary_secondary;  // small qualifier line right of primary
    lv_obj_t *extras_label[4];
    lv_obj_t *extras_value[4];
    char last_primary[24];
    char last_primary_secondary[24];
    char last_extras[4][24];
    MetricBinding metric;  // copy of metrics[0]
};

static lv_obj_t *create_hero_plus(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    HeroPlusState *st = (HeroPlusState *)heap_caps_calloc(1, sizeof(HeroPlusState),
                                                          MALLOC_CAP_INTERNAL);
    if (!st) { net::logf("[layout] hero_plus alloc failed"); return root; }
    st->metric = spec.metrics[0];
    strncpy(st->last_primary, "\xFF", sizeof(st->last_primary));
    strncpy(st->last_primary_secondary, "\xFF", sizeof(st->last_primary_secondary));
    for (int i = 0; i < 4; ++i)
        strncpy(st->last_extras[i], "\xFF", sizeof(st->last_extras[0]));

    // Title at top, montserrat_28, accent color.
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, spec.title ? spec.title : st->metric.label);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(st->metric.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Hero panel (top half).
    lv_obj_t *hero = lv_obj_create(root);
    lv_obj_set_size(hero, LCD_W - 16, 220);
    lv_obj_set_pos(hero, 8, 58);
    lv_obj_set_style_bg_color(hero, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 10, 0);
    lv_obj_set_style_pad_all(hero, 0, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    st->primary_value = lv_label_create(hero);
    lv_label_set_text(st->primary_value, "--");
    lv_obj_set_style_text_font(st->primary_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->primary_value, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->primary_value, LV_ALIGN_CENTER, -30, -10);

    if (st->metric.unit && st->metric.unit[0]) {
        st->primary_unit = lv_label_create(hero);
        lv_label_set_text(st->primary_unit, st->metric.unit);
        lv_obj_set_style_text_font(st->primary_unit, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(st->primary_unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(st->primary_unit, LV_ALIGN_CENTER, 80, 8);
    }

    st->primary_secondary = lv_label_create(hero);
    lv_label_set_text(st->primary_secondary, "");
    lv_obj_set_style_text_font(st->primary_secondary, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->primary_secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->primary_secondary, LV_ALIGN_BOTTOM_MID, 0, -10);

    // Secondary stat panel - 2x2 grid below hero.
    lv_obj_t *grid = lv_obj_create(root);
    int gy = 58 + 220 + 8;
    lv_obj_set_size(grid, LCD_W - 16, LCD_H - gy - 8);
    lv_obj_set_pos(grid, 8, gy);
    lv_obj_set_style_bg_color(grid, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(grid, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(grid, 1, 0);
    lv_obj_set_style_radius(grid, 10, 0);
    lv_obj_set_style_pad_all(grid, 12, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int cell_w = (LCD_W - 16 - 24) / 2;
    int cell_h = ((LCD_H - gy - 8) - 24) / 2;
    for (uint8_t i = 0; i < st->metric.extras_count && i < 4; ++i) {
        int col = i % 2;
        int row = i / 2;
        int x = col * cell_w;
        int y = row * cell_h;
        st->extras_label[i] = lv_label_create(grid);
        lv_label_set_text(st->extras_label[i],
                          st->metric.extras[i].label ? st->metric.extras[i].label : "");
        lv_obj_set_style_text_font(st->extras_label[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st->extras_label[i], lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_pos(st->extras_label[i], x, y);

        st->extras_value[i] = lv_label_create(grid);
        lv_label_set_text(st->extras_value[i], "--");
        lv_obj_set_style_text_font(st->extras_value[i], &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(st->extras_value[i], lv_color_hex(theme.fg), 0);
        lv_obj_set_pos(st->extras_value[i], x, y + 18);
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_hero_plus(lv_obj_t *root, const ScreenVariantSpec &spec,
                             const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (HeroPlusState *)lv_obj_get_user_data(root);
    if (!st) return;

    char pri[24], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->primary_value, st->last_primary,
                            sizeof(st->last_primary), pri);
    if (st->primary_secondary) {
        ui::set_text_if_changed(st->primary_secondary, st->last_primary_secondary,
                                sizeof(st->last_primary_secondary), sec);
    }

    for (uint8_t i = 0; i < st->metric.extras_count && i < 4; ++i) {
        if (!st->extras_value[i]) continue;
        MetricBinding eb = {};
        eb.source = st->metric.extras[i].source;
        char ep[24], esec[24];
        format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
        ui::set_text_if_changed(st->extras_value[i], st->last_extras[i],
                                sizeof(st->last_extras[i]), ep);
    }
}

// ---------------------------------------------------------------------------
// status_list template - vertical list of label : value rows. Up to 8
// rows; each metric is one row. Used for system/diagnostics screens.

struct StatusListState {
    lv_obj_t *value_labels[8];
    char last_values[8][32];
};

static lv_obj_t *create_status_list(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    StatusListState *st = (StatusListState *)heap_caps_calloc(1, sizeof(StatusListState),
                                                              MALLOC_CAP_INTERNAL);
    if (!st) { net::logf("[layout] status_list alloc failed"); return root; }
    for (int i = 0; i < 8; ++i)
        strncpy(st->last_values[i], "\xFF", sizeof(st->last_values[0]));

    // Title at top.
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, spec.title ? spec.title : "STATUS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Panel container.
    lv_obj_t *panel = lv_obj_create(root);
    lv_obj_set_size(panel, LCD_W - 16, LCD_H - 60);
    lv_obj_set_pos(panel, 8, 52);
    lv_obj_set_style_bg_color(panel, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    int row_h = (LCD_H - 60 - 24) / 8;
    int n = spec.metric_count > 8 ? 8 : spec.metric_count;
    for (int i = 0; i < n; ++i) {
        const MetricBinding &m = spec.metrics[i];
        lv_obj_t *lbl = lv_label_create(panel);
        lv_label_set_text(lbl, m.label ? m.label : "");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_pos(lbl, 0, i * row_h);

        st->value_labels[i] = lv_label_create(panel);
        lv_label_set_text(st->value_labels[i], "--");
        lv_obj_set_style_text_font(st->value_labels[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(st->value_labels[i], lv_color_hex(theme.fg), 0);
        lv_obj_set_pos(st->value_labels[i], 180, i * row_h);
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_status_list(lv_obj_t *root, const ScreenVariantSpec &spec,
                                const sk::Data &data) {
    if (!root) return;
    auto *st = (StatusListState *)lv_obj_get_user_data(root);
    if (!st) return;
    int n = spec.metric_count > 8 ? 8 : spec.metric_count;
    for (int i = 0; i < n; ++i) {
        if (!st->value_labels[i]) continue;
        char pri[24], sec[24];
        format_metric(spec.metrics[i], data, pri, sizeof(pri), sec, sizeof(sec));
        char combined[32];
        if (spec.metrics[i].unit && spec.metrics[i].unit[0]) {
            snprintf(combined, sizeof(combined), "%s %s", pri, spec.metrics[i].unit);
        } else {
            snprintf(combined, sizeof(combined), "%s", pri);
        }
        ui::set_text_if_changed(st->value_labels[i], st->last_values[i],
                                sizeof(st->last_values[i]), combined);
    }
}

// ---------------------------------------------------------------------------
// round_instrument template - circular bezel with center value. Minimal
// first cut (no rotating needle yet); useful for compass/heading
// digital readouts where a future revision can layer the needle on top.

struct RoundInstrumentState {
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *secondary;
    char last_value[24];
    char last_secondary[24];
    MetricBinding metric;
};

static lv_obj_t *create_round_instrument(lv_obj_t *parent,
                                          const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    RoundInstrumentState *st = (RoundInstrumentState *)heap_caps_calloc(
        1, sizeof(RoundInstrumentState), MALLOC_CAP_INTERNAL);
    if (!st) { net::logf("[layout] round_inst alloc failed"); return root; }
    st->metric = spec.metrics[0];
    strncpy(st->last_value, "\xFF", sizeof(st->last_value));
    strncpy(st->last_secondary, "\xFF", sizeof(st->last_secondary));

    // Title at top.
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, spec.title ? spec.title : st->metric.label);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(st->metric.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Bezel: a rounded square that approximates a circle on a square panel.
    int dial_size = LCD_W - 80;
    lv_obj_t *bezel = lv_obj_create(root);
    lv_obj_set_size(bezel, dial_size, dial_size);
    lv_obj_align(bezel, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(bezel, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bezel, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(bezel, 4, 0);
    lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(bezel, 0, 0);
    lv_obj_clear_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);

    st->value = lv_label_create(bezel);
    lv_label_set_text(st->value, "--");
    lv_obj_set_style_text_font(st->value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->value, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->value, LV_ALIGN_CENTER, 0, -8);

    if (st->metric.unit && st->metric.unit[0]) {
        st->unit = lv_label_create(bezel);
        lv_label_set_text(st->unit, st->metric.unit);
        lv_obj_set_style_text_font(st->unit, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(st->unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(st->unit, LV_ALIGN_CENTER, 0, 32);
    }

    st->secondary = lv_label_create(bezel);
    lv_label_set_text(st->secondary, "");
    lv_obj_set_style_text_font(st->secondary, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->secondary, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_round_instrument(lv_obj_t *root, const ScreenVariantSpec &spec,
                                     const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (RoundInstrumentState *)lv_obj_get_user_data(root);
    if (!st) return;
    char pri[24], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->value, st->last_value, sizeof(st->last_value), pri);
    if (st->secondary) {
        ui::set_text_if_changed(st->secondary, st->last_secondary,
                                sizeof(st->last_secondary), sec);
    }
}

// ---------------------------------------------------------------------------
// split_pair template - two large metrics side by side.
// 480x480 layout: each half is 234 px wide; metric primary value
// montserrat_48 centred, caption above, unit beside primary,
// secondary line (small) below.

struct SplitHalf {
    lv_obj_t *cap;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *secondary;
    char last_value[24];
    char last_secondary[24];
    MetricBinding metric;
};

struct SplitPairState {
    SplitHalf left;
    SplitHalf right;
};

static void split_half_build(lv_obj_t *parent, int x, int y, int w, int h,
                             const MetricBinding &m, SplitHalf &out) {
    out.metric = m;
    strncpy(out.last_value, "\xFF", sizeof(out.last_value));
    strncpy(out.last_secondary, "\xFF", sizeof(out.last_secondary));

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // Accent rail down the inner edge.
    lv_obj_t *rail = lv_obj_create(panel);
    lv_obj_set_size(rail, 4, h - 28);
    lv_obj_set_pos(rail, 0, 14);
    lv_obj_set_style_bg_color(rail, lv_color_hex(m.accent), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 2, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    out.cap = lv_label_create(panel);
    lv_label_set_text(out.cap, m.label ? m.label : "");
    lv_obj_set_style_text_font(out.cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(out.cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(out.cap, LV_ALIGN_TOP_MID, 0, 12);

    out.value = lv_label_create(panel);
    lv_label_set_text(out.value, "--");
    lv_obj_set_style_text_font(out.value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(out.value, lv_color_hex(theme.fg), 0);
    lv_obj_align(out.value, LV_ALIGN_CENTER, -10, 0);

    if (m.unit && m.unit[0]) {
        out.unit = lv_label_create(panel);
        lv_label_set_text(out.unit, m.unit);
        lv_obj_set_style_text_font(out.unit, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(out.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(out.unit, LV_ALIGN_CENTER, 60, 12);
    } else {
        out.unit = nullptr;
    }

    out.secondary = lv_label_create(panel);
    lv_label_set_text(out.secondary, "");
    lv_obj_set_style_text_font(out.secondary, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(out.secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(out.secondary, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static lv_obj_t *create_split_pair(lv_obj_t *parent,
                                    const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    SplitPairState *st = (SplitPairState *)heap_caps_calloc(
        1, sizeof(SplitPairState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] split_pair alloc failed");
        return root;
    }

    // Title across top.
    if (spec.title && spec.title[0]) {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, spec.title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    }

    // Two panels share the screen below the MOB-safe band.
    const int top_y = 64;
    const int bottom_margin = 8;
    const int gap = 8;
    int half_w = (LCD_W - 16 - gap) / 2;
    int h = LCD_H - top_y - bottom_margin;
    int left_x = 8;
    int right_x = left_x + half_w + gap;

    split_half_build(root, left_x, top_y, half_w, h, spec.metrics[0],
                     st->left);
    if (spec.metric_count >= 2) {
        split_half_build(root, right_x, top_y, half_w, h, spec.metrics[1],
                         st->right);
    } else {
        // single-metric mode: stretch left to full width
        lv_obj_set_size(lv_obj_get_child(root, 1), LCD_W - 16, h);
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_split_pair(lv_obj_t *root, const ScreenVariantSpec &spec,
                              const sk::Data &data) {
    if (!root) return;
    auto *st = (SplitPairState *)lv_obj_get_user_data(root);
    if (!st) return;

    auto update_half = [&](SplitHalf &h) {
        if (!h.value) return;
        char pri[24], sec[24];
        format_metric(h.metric, data, pri, sizeof(pri), sec, sizeof(sec));
        ui::set_text_if_changed(h.value, h.last_value, sizeof(h.last_value), pri);
        if (h.secondary) {
            ui::set_text_if_changed(h.secondary, h.last_secondary,
                                    sizeof(h.last_secondary), sec);
        }
    };
    update_half(st->left);
    if (spec.metric_count >= 2) update_half(st->right);
}

// ---------------------------------------------------------------------------
// Public factory entry points

lv_obj_t *create(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    switch (spec.template_id) {
    case TemplateId::QuadGrid:        return create_quad_grid(parent, spec);
    case TemplateId::HeroPlus:        return create_hero_plus(parent, spec);
    case TemplateId::StatusList:      return create_status_list(parent, spec);
    case TemplateId::RoundInstrument: return create_round_instrument(parent, spec);
    case TemplateId::SplitPair:       return create_split_pair(parent, spec);
    default:
        net::logf("[layout] template %d not implemented yet", (int)spec.template_id);
        return nullptr;
    }
}

void update(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    switch (spec.template_id) {
    case TemplateId::QuadGrid:        update_quad_grid(root, spec, data); break;
    case TemplateId::HeroPlus:        update_hero_plus(root, spec, data); break;
    case TemplateId::StatusList:      update_status_list(root, spec, data); break;
    case TemplateId::RoundInstrument: update_round_instrument(root, spec, data); break;
    case TemplateId::SplitPair:       update_split_pair(root, spec, data); break;
    default: break;
    }
}

}  // namespace ui::layouts
