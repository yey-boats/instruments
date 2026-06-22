#include "ui_layouts.h"

#include "layout.h"
#include "midl_limits.h"  // midl::FirmwareLimits (spec-derived tile bound), budget caps
#include "subscription_set.h"
#include "ui_theme.h"
#include "config_runtime.h"
#include "ui_data.h"
#include "value_format.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "ui_markers.h"
#include "signalk.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "app_events.h"
#include "net.h"
#include "autopilot.h"
#include "beeper.h"

#include "storage.h"

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
    lv_obj_t *extras[4];  // multi-row tiles - small label+value lines
    // Per-kind auxiliary widgets (compass needle, gauge arc, bar, etc.).
    // Painter sets only the slots it needs; updater uses them via `kind`.
    lv_obj_t *aux;           // primary aux widget (arc / bar / ring)
    lv_obj_t *aux2;          // secondary aux (gauge needle / autopilot pill)
    ui::MarkerRing markers;  // compass steering markers (HDG/COG/CTS)
    char last_value[24];
    char last_secondary[24];
    char last_extras[4][32];
    int last_aux_pct;  // last gauge/bar value (0..100); -1 = unset
    int idx;           // metric index into spec.metrics[]
    WidgetKind kind;
};

struct QuadGridState {
    QuadGridTile tiles[4];
};

// Reverse of layout_renderer's path_to_source(): the canonical SignalK path a
// MetricSource consumes. Defined unconditionally so the per-screen subscription
// manager (collect_paths) and the perf-counter store shadow share one source of
// truth. Position and APState ARE real paths a screen subscribes; None maps to
// nullptr (skipped by callers).
const char *source_to_path(MetricSource s) {
    switch (s) {
    case MetricSource::AWS_kn:
        return "environment.wind.speedApparent";
    case MetricSource::AWA_deg:
        return "environment.wind.angleApparent";
    case MetricSource::TWS_kn:
        return "environment.wind.speedTrue";
    case MetricSource::TWA_deg:
        return "environment.wind.angleTrueWater";
    case MetricSource::SOG_kn:
        return "navigation.speedOverGround";
    case MetricSource::COG_deg:
        return "navigation.courseOverGroundTrue";
    case MetricSource::HDG_deg:
        return "navigation.headingTrue";
    case MetricSource::Depth_m:
        return "environment.depth.belowTransducer";
    case MetricSource::DepthKeel_m:
        return "environment.depth.belowKeel";
    case MetricSource::WaterTemp_C:
        return "environment.water.temperature";
    case MetricSource::BatteryV:
        return "electrical.batteries.house.voltage";
    case MetricSource::BatterySOC_pct:
        return "electrical.batteries.house.stateOfCharge";
    case MetricSource::DTW:
        return "navigation.courseRhumbline.nextPoint.distance";
    case MetricSource::BTW_deg:
        return "navigation.courseRhumbline.nextPoint.bearingTrue";
    case MetricSource::CTS_deg:
        return "navigation.courseRhumbline.bearingTrackTrue";
    case MetricSource::XTE:
        return "navigation.courseRhumbline.crossTrackError";
    case MetricSource::VMG_kn:
        return "navigation.courseRhumbline.velocityMadeGood";
    case MetricSource::Rudder_deg:
        return "steering.rudderAngle";
    case MetricSource::Position:
        return "navigation.position";
    case MetricSource::APState:
        return "steering.autopilot.state";
    case MetricSource::None:
    default:
        return nullptr;
    }
}

void collect_paths(const ScreenVariantSpec &spec, sk::SubscriptionSet &out) {
    if (!spec.metrics) return;
    for (uint8_t i = 0; i < spec.metric_count; ++i) {
        const MetricBinding &m = spec.metrics[i];
        const char *p = source_to_path(m.source);
        if (p) out.add(p);
        uint8_t ne = m.extras_count;
        if (ne > 4) ne = 4;  // bound to the fixed extras[] array
        for (uint8_t e = 0; e < ne; ++e) {
            const char *pe = source_to_path(m.extras[e].source);
            if (pe) out.add(pe);
        }
    }
}

#ifdef DBG_PERF_COUNTERS
// Benchmark: when on, built-in screens shadow-resolve each metric through the
// dynamic PathStore (the cost a real "port built-ins to the store" would add),
// so bench-sweep can A/B the typed fast-path vs. the store path.
bool g_bench_store_mode = false;
#endif  // DBG_PERF_COUNTERS

// ---------------------------------------------------------------------------
// Metric formatting. Returns the primary value text, and optionally
// fills `secondary` with a short context string. NaN -> "--".

// Display formatting (decimals + k/M scaling per unit class). Refreshed once
// per update() from config; format_metric reads it. All painting runs on the
// single LVGL task, so this file-static needs no lock.
static config::FormatConfig s_fmt;

static void format_metric(const MetricBinding &m, const sk::Data &d, char *primary, size_t pcap,
                          char *secondary, size_t scap) {
    secondary[0] = 0;
#ifdef DBG_PERF_COUNTERS
    if (g_bench_store_mode && m.source != MetricSource::None) {
        const char *p = source_to_path(m.source);
        if (p) {
            volatile double x = sk::dynamicStore().get(p);  // incur store-port cost
            (void)x;
        }
    }
#endif
    switch (m.source) {
    case MetricSource::AWS_kn:
        vfmt::format_scaled(mps_to_kn(d.aws), s_fmt.speed, primary, pcap);
        if (!isnan(d.awa)) {
            double deg = rad_to_deg_pos(d.awa);
            bool stbd = deg <= 180.0;
            snprintf(secondary, scap, "%.0f%c", stbd ? deg : 360 - deg, stbd ? 'S' : 'P');
        }
        break;
    case MetricSource::TWS_kn:
        vfmt::format_scaled(mps_to_kn(d.tws), s_fmt.speed, primary, pcap);
        if (!isnan(d.twa)) {
            double deg = rad_to_deg_pos(d.twa);
            bool stbd = deg <= 180.0;
            snprintf(secondary, scap, "%.0f%c", stbd ? deg : 360 - deg, stbd ? 'S' : 'P');
        }
        break;
    case MetricSource::AWA_deg:
        if (!isnan(d.awa)) {
            double deg = rad_to_deg_pos(d.awa);
            bool stbd = deg <= 180.0;
            snprintf(primary, pcap, "%.0f%c", stbd ? deg : 360 - deg, stbd ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::TWA_deg:
        if (!isnan(d.twa)) {
            double deg = rad_to_deg_pos(d.twa);
            bool stbd = deg <= 180.0;
            snprintf(primary, pcap, "%.0f%c", stbd ? deg : 360 - deg, stbd ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::SOG_kn:
        vfmt::format_scaled(mps_to_kn(d.sog), s_fmt.speed, primary, pcap);
        break;
    case MetricSource::COG_deg:
        if (!isnan(d.cogTrue))
            snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.cogTrue));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::HDG_deg:
        if (!isnan(d.headingTrue))
            snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.headingTrue));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::Depth_m:
        vfmt::format_scaled(d.depth, s_fmt.depth, primary, pcap);
        break;
    case MetricSource::DepthKeel_m:
        vfmt::format_scaled(d.depthKeel, s_fmt.depth, primary, pcap);
        break;
    case MetricSource::WaterTemp_C:
        vfmt::format_scaled(isnan(d.waterTemp) ? NAN : k_to_c(d.waterTemp), s_fmt.temperature,
                            primary, pcap);
        break;
    case MetricSource::BatteryV:
        vfmt::format_scaled(d.battVoltage, s_fmt.voltage, primary, pcap);
        if (!isnan(d.battSoc)) {
            snprintf(secondary, scap, "%.0f%%", d.battSoc * 100.0);
        }
        break;
    case MetricSource::BatterySOC_pct:
        if (!isnan(d.battSoc))
            snprintf(primary, pcap, "%.0f%%", d.battSoc * 100.0);
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::DTW:
        // Display in nm (the static unit label is "nm"); k/M scaling per the
        // distance format keeps large legs fitting the tile (1234.5 -> 1.23k).
        vfmt::format_scaled(isnan(d.dtw) ? NAN : d.dtw / 1852.0, s_fmt.distance, primary, pcap);
        break;
    case MetricSource::BTW_deg:
        if (!isnan(d.btw))
            snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.btw));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::CTS_deg:
        if (!isnan(d.cts))
            snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.cts));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::XTE:
        // Cross-track error in NAUTICAL MILES (the unit a steering screen wants,
        // not raw meters) with a port/starboard suffix (matches the wind-angle
        // P/S convention; the big hero font font_xl_64 has no '+' glyph, so a
        // signed "%+.2f" rendered a tofu box). +ve = right of track (steer
        // port -> 'P'), -ve = left of track (steer starboard -> 'S'). The static
        // unit label is "nm". Two decimals (~0.01 nm ≈ 19 m) resolves close-
        // quarters steering. No '>' over-range token here: font_xl_64 has no '>'
        // glyph (renders tofu); the operator's off-scale cue lives on the XTE
        // strip (build_xte_strip / format_xte), which uses a normal font.
        if (!isnan(d.xte)) {
            constexpr double kMetersPerNm = 1852.0;
            snprintf(primary, pcap, "%.2f%c", fabs(d.xte) / kMetersPerNm, d.xte >= 0 ? 'P' : 'S');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::VMG_kn:
        vfmt::format_scaled(mps_to_kn(d.vmg), s_fmt.speed, primary, pcap);
        break;
    case MetricSource::Rudder_deg:
        // Rudder angle: magnitude in degrees + port/starboard helm suffix
        // (+ve steering.rudderAngle = starboard). Centered "0" reads bare.
        if (!isnan(d.rudder)) {
            double deg = d.rudder * 180.0 / M_PI;
            double mag = fabs(deg);
            char num[16];
            vfmt::format_scaled(mag, s_fmt.angle, num, sizeof(num));
            if (mag < 0.5)
                snprintf(primary, pcap, "%s", num);
            else
                snprintf(primary, pcap, "%s%c", num, deg > 0 ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::Position:
        if (!isnan(d.lat) && !isnan(d.lon)) {
            format_position(d.lat, d.lon, pos_format(), primary, pcap);
        } else {
            snprintf(primary, pcap, "no fix");
        }
        break;
    case MetricSource::APState:
        if (d.apState[0])
            snprintf(primary, pcap, "%s", d.apState);
        else
            snprintf(primary, pcap, "off");
        break;
    case MetricSource::None:
    default:
        snprintf(primary, pcap, "--");
        break;
    }
}

// ---- tap-to-zoom target --------------------------------------------------
// Set by a tile tap; consumed by the dedicated "zoom" screen (see ui::zoom
// below) which renders this single metric full-screen. File-scope so both
// the tap callback (here) and the zoom screen (same TU) share it.
static MetricBinding g_zoom_target = {"", "", "", MetricSource::None, 0, nullptr};

// Public entry so a controller (or the sim harness) can drive the zoom screen
// to a chosen metric without a physical tile tap.
void set_zoom_target(const MetricBinding &m) {
    g_zoom_target = m;
}

// Slice 6: per-field zoom dispatch. Honors the authored tile's `zoomable` /
// `zoom_target`: "auto"/"" scales the field in place on the dedicated zoom
// screen (reusing g_zoom_target), a "<screenId>" target switches to that full
// screen via show_by_id. Legacy/hardcoded bindings (zoom_target == NULL) keep
// the original always-auto-scale behavior so built-in dashboards are unchanged.
// Runs on the LVGL/UI task (LV_EVENT_CLICKED dispatch); g_zoom_target is the
// shared function-static scratch, so no large MetricBinding goes on this stack.
static void tile_zoom_action_cb(lv_event_t *e) {
    const MetricBinding *m = (const MetricBinding *)lv_event_get_user_data(e);
    if (!m) return;

    // No explicit zoom config (built-in screen tile): preserve the legacy
    // auto-zoom for any real metric.
    if (!m->zoom_target) {
        if (m->source == MetricSource::None) return;
        g_zoom_target = *m;
        ui::show_by_id("zoom");
        return;
    }

    switch (layout::zoom_action(m->zoomable, m->zoom_target)) {
    case layout::ZOOM_AUTO_SCALE:
        if (m->source == MetricSource::None) return;
        g_zoom_target = *m;
        ui::show_by_id("zoom");
        break;
    case layout::ZOOM_SHOW_SCREEN:
        // show_by_id validates the id and no-ops (returns false) on a dangling
        // screen ref, so a stale zoom target is harmless.
        ui::show_by_id(m->zoom_target);
        break;
    case layout::ZOOM_NONE:
    default:
        break;
    }
}

// Numeric value for chart-able metrics, in display units. Returns NaN
// for non-scalar bindings (Position, APState, etc).
static double metric_scalar(const MetricBinding &m, const sk::Data &d) {
    switch (m.source) {
    case MetricSource::AWS_kn:
        return isnan(d.aws) ? NAN : mps_to_kn(d.aws);
    case MetricSource::TWS_kn:
        return isnan(d.tws) ? NAN : mps_to_kn(d.tws);
    case MetricSource::SOG_kn:
        return isnan(d.sog) ? NAN : mps_to_kn(d.sog);
    case MetricSource::Depth_m:
        return d.depth;
    case MetricSource::DepthKeel_m:
        return d.depthKeel;
    case MetricSource::WaterTemp_C:
        return isnan(d.waterTemp) ? NAN : k_to_c(d.waterTemp);
    case MetricSource::BatteryV:
        return d.battVoltage;
    case MetricSource::BatterySOC_pct:
        return isnan(d.battSoc) ? NAN : d.battSoc * 100.0;
    case MetricSource::VMG_kn:
        return isnan(d.vmg) ? NAN : mps_to_kn(d.vmg);
    case MetricSource::COG_deg:
        return isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    case MetricSource::HDG_deg:
        return isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
    case MetricSource::AWA_deg:
        return isnan(d.awa) ? NAN : rad_to_deg_pos(d.awa);
    case MetricSource::TWA_deg:
        return isnan(d.twa) ? NAN : rad_to_deg_pos(d.twa);
    case MetricSource::BTW_deg:
        return isnan(d.btw) ? NAN : rad_to_deg_pos(d.btw);
    case MetricSource::CTS_deg:
        return isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    case MetricSource::XTE:
        return d.xte;
    case MetricSource::DTW:
        return isnan(d.dtw) ? NAN : d.dtw / 1852.0;  // nm
    case MetricSource::Rudder_deg:
        return isnan(d.rudder) ? NAN : d.rudder * 180.0 / M_PI;  // signed deg
    default:
        return NAN;
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

// Button element tap handler (MIDL `action`). user_data is the tile's
// MetricBinding* (stable: it points into the screen's arena/spec). A command
// action funnels its target through net::dispatchCommand on the UI task — the
// same path screen_settings uses for `theme`/`demo`, so it is safe here (the
// funnel posts SignalK PUTs onto the net worker itself; it does no blocking
// I/O on this task). A nav action posts ShowScreen, mirroring tile_clicked_cb.
static void button_action_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const MetricBinding *m = (const MetricBinding *)lv_event_get_user_data(e);
    if (!m) return;
    if (m->command && m->command[0]) {
        net::logf("[layout] button command='%s'", m->command);
        net::dispatchCommand(m->command);
        return;
    }
    if (m->target_screen && m->target_screen[0]) {
        net::logf("[layout] button nav target=%s", m->target_screen);
        app::Command c;
        c.type = app::CommandType::ShowScreen;
        strncpy(c.a, m->target_screen, sizeof(c.a) - 1);
        c.t_post_us = micros();
        app::post(c, 0);
    }
}

// Map a scalar source value to a 0..1 fraction for gauge/bar widgets.
// Heuristic per-source ranges; widgets that don't have an obvious range
// (e.g., heading angles, positions) return NAN and render as empty.
static double scalar_unit_fraction(MetricSource src, double v) {
    if (isnan(v)) return NAN;
    auto clamp01 = [](double x) {
        if (x < 0) return 0.0;
        if (x > 1) return 1.0;
        return x;
    };
    switch (src) {
    case MetricSource::BatterySOC_pct:
        return clamp01(v / 100.0);
    case MetricSource::BatteryV:
        return clamp01((v - 11.0) / (14.4 - 11.0));
    case MetricSource::Depth_m:
    case MetricSource::DepthKeel_m:
        return clamp01(v / 30.0);
    case MetricSource::AWS_kn:
        return clamp01(v / 40.0);
    case MetricSource::TWS_kn:
        return clamp01(v / 40.0);
    case MetricSource::SOG_kn:
        return clamp01(v / 15.0);
    case MetricSource::VMG_kn:
        return clamp01(v / 15.0);
    case MetricSource::WaterTemp_C:
        return clamp01((v - 5.0) / (30.0 - 5.0));
    default:
        return NAN;
    }
}

// Gauge/bar fill fraction (0..1) for one binding. When the binding carries an
// explicit MIDL `format.range` (range_min != range_max), scale v into that
// [min,max] window; otherwise fall back to the built-in per-source heuristic so
// legacy tiles (range_min==range_max==0) keep their current behavior byte-for-byte.
static double binding_unit_fraction(const MetricBinding &m, double v) {
    if (m.range_min == m.range_max) return scalar_unit_fraction(m.source, v);
    if (isnan(v)) return NAN;
    double lo = m.range_min, hi = m.range_max;
    if (hi == lo) return NAN;  // defensive; range_min!=range_max already checked
    double f = (v - lo) / (hi - lo);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

// --- Per-kind body painters --------------------------------------------
// Every painter is called after the panel/border/accent-rail/caption
// chrome is already in place. They populate the t.value / t.unit /
// t.secondary / t.aux slots their updater will reach for.

// Layout strategy: the chrome caption already lives at the tile's
// top-left (handled by build_tile). Inside the body we mirror the
// editor's `.num-stack` flex-column-center: value+unit row centered,
// secondary line directly below it. Secondary tracks the value rather
// than pinning to a corner so the label moves with the number when the
// number's pixel width changes ("9.7" vs "12.74"). Value size scales
// with tile area so the number takes ~60% of the vertical content
// region (the "75% of dedicated space" rule in the goal).
// Shrink the hero value font to the largest ladder step whose rendered width
// fits `max_w`. Always starts from the biggest font so the value grows back
// when it shortens. Fixes wide scaled strings (e.g. "68.07kS", "453.95")
// overflowing/clipping QuadGrid tiles. Called only on text change.
static void fit_value_font(lv_obj_t *label, const char *txt, int max_w) {
    static const lv_font_t *const ladder[] = {&font_xl_64, &lv_font_montserrat_48,
                                              &lv_font_montserrat_38, &lv_font_montserrat_28};
    for (int i = 0; i < 4; ++i) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, ladder[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) {
            lv_obj_set_style_text_font(label, ladder[i], 0);
            return;
        }
    }
    lv_obj_set_style_text_font(label, ladder[3], 0);
}

static void paint_numeric_body(QuadGridTile &t, const MetricBinding &m, int w, int h) {
    bool has_extras = (m.extras_count > 0);

    // Pick value font based on tile height. On 480x480 sunton the quad grid
    // gives ~200 px tiles -> the custom 64px font (hero). Single-field tiles
    // without a secondary/extras line get the big font; tighter tiles step
    // down 48 -> 38 -> 28.
    const lv_font_t *vfont = &font_xl_64;
    if (h < 180 || has_extras) vfont = &lv_font_montserrat_48;
    if (h < 150) vfont = &lv_font_montserrat_38;
    if (h < 110) vfont = &lv_font_montserrat_28;
    // Unit font: only 14/20 enabled to keep LVGL glyph cache off
    // internal heap (16/18 sizes blew int_largest from 7668 -> 1908 bytes
    // and starved the network task; see USB-flash diagnostic).
    const lv_font_t *ufont =
        (vfont == &lv_font_montserrat_28) ? &lv_font_montserrat_14 : &lv_font_montserrat_20;

    // Flex row container holds value + unit so they're true bottom-aligned
    // (baselines match for digit fonts since there are no descenders).
    // Previous LV_ALIGN_OUT_RIGHT_BOTTOM put the unit's TOP at the value's
    // BOTTOM corner which made the unit drop below the digits and crowd
    // the next character - that's the overlap users saw.
    lv_obj_t *row = lv_obj_create(t.root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, has_extras ? -28 : -8);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    t.value = lv_label_create(row);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    if (m.unit && m.unit[0]) {
        t.unit = lv_label_create(row);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, ufont, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        // The flex row's cross-axis ALIGN_END pulls the unit to the
        // bottom edge of the row's content box - same baseline as the
        // taller value since digits have no descenders.
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }

    if (!has_extras) {
        // Secondary tracks the value: it sits directly below, centered.
        // This is the editor's `.num-secondary` placement, NOT the prior
        // bottom-right corner pin. The label follows the number's
        // horizontal center even as the number widens/shrinks.
        t.secondary = lv_label_create(t.root);
        lv_label_set_text(t.secondary, "");
        lv_obj_set_style_text_font(t.secondary, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align_to(t.secondary, row, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // Multi-row tile: extras stacked below the (shrunken) value.
        // Center extras horizontally; spacing scales with tile height.
        int row_h = (h < 160) ? 16 : 20;
        int first_y = (h < 160) ? 60 : 76;
        (void)w;
        for (uint8_t i = 0; i < m.extras_count && i < 4; ++i) {
            t.extras[i] = lv_label_create(t.root);
            lv_label_set_text(t.extras[i], "");
            lv_obj_set_style_text_font(t.extras[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(t.extras[i], lv_color_hex(theme.fg), 0);
            lv_obj_align(t.extras[i], LV_ALIGN_TOP_MID, 0, first_y + i * row_h);
            lv_obj_clear_flag(t.extras[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// Center, in t.root *content* coordinates, of a dial placed with
// LV_ALIGN_CENTER, 0, +4 on a style_panel()'d tile. style_panel adds a
// border + padding inset, so LV_ALIGN_CENTER centers the dial inside the
// content box (w - 2*inset wide), while lv_obj_set_pos() in make_holder is
// ALSO content-relative. Passing the raw tile center (w/2, h/2) to
// build_marker_ring therefore lands the marker pivot `inset` px down-and-right
// of the dial center, decentering the orbiting glyphs. Compute the matching
// content-area center so the marker ring is concentric with the dial.
static inline void dial_center(int w, int h, int &cx, int &cy) {
    const int inset = chrome::panel_border + chrome::panel_pad;
    cx = (w - 2 * inset) / 2;
    cy = (h - 2 * inset) / 2 + 4;  // matches the dial's LV_ALIGN_CENTER, 0, +4
}

// Compass widget: round bezel with accent border, heading number in the
// center, small "▲" marker at top, CTS label at bottom. Mirrors editor
// .wpreview .compass.
static void paint_compass_body(QuadGridTile &t, const MetricBinding &m, int w, int h) {
    // Ring size: reserve top 24 px for chrome caption + marker, bottom
    // 22 px for CTS label. Diameter = min(w,h) - 56 keeps the heading
    // text from colliding with cardinals at 14 px.
    int dia = (w < h ? w : h) - 56;
    if (dia < 72) dia = 72;
    lv_obj_t *ring = lv_obj_create(t.root);
    lv_obj_set_size(ring, dia, dia);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(ring, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    t.aux = ring;

    // Steering marker set: HDG (solid triangle), COG (hollow triangle),
    // CTS (solid diamond, alarm/red). Bearings filled live in update from
    // metric_scalar(). Amber is reserved for the AP-target / TWD diamonds so
    // CTS and target stay distinguishable on the AP HUD.
    ui::MarkerSpec steer_markers[3] = {
        {NAN, ui::Glyph::Triangle, true, theme.accent},
        {NAN, ui::Glyph::Triangle, false, theme.good},
        {NAN, ui::Glyph::Diamond, true, theme.alarm},
    };
    int mcx, mcy;
    dial_center(w, h, mcx, mcy);
    t.markers = ui::build_marker_ring(t.root, mcx, mcy, dia / 2, steer_markers, 3,
                                      /*occlude_lower=*/false);

    // N/E/S/W cardinals: padded in from the ring border so they don't
    // visually merge with the heading number (which is centered).
    static const char *cardinals[4] = {"N", "E", "S", "W"};
    static const lv_align_t aligns[4] = {LV_ALIGN_TOP_MID, LV_ALIGN_RIGHT_MID, LV_ALIGN_BOTTOM_MID,
                                         LV_ALIGN_LEFT_MID};
    // E/W sit at the ring edge with only a hair of inset so the centre number
    // (over its vignette disc) never reaches them.
    static const int xs[4] = {0, -2, 0, 2};
    static const int ys[4] = {4, 0, -4, 0};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *c = lv_label_create(ring);
        lv_label_set_text(c, cardinals[i]);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(i == 0 ? theme.fg : theme.fg_dim), 0);
        lv_obj_align(c, aligns[i], xs[i], ys[i]);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    }

    // Center heading - the dominant element. With the direction marker now
    // outside the ring, the centre number can fill the dial: big tiles get the
    // 64px hero font, stepping down on smaller rings.
    const lv_font_t *vfont = &lv_font_montserrat_38;
    if (dia >= 110) vfont = &lv_font_montserrat_48;
    if (dia >= 135) vfont = &font_xl_64;
    if (dia < 90) vfont = &lv_font_montserrat_28;

    // Vignette disc behind the centre number: a soft panel-coloured pad so the
    // wide HDG digits (e.g. "359") read on top of, not into, the W/E cardinals
    // that sit at the ring edge. Sized to the central third of the dial; the
    // number paints over it. Radius keeps it clear of the E/W letters' inset.
    {
        int disc = dia * 0.56;
        if (disc < 56) disc = 56;
        lv_obj_t *vig = lv_obj_create(ring);
        lv_obj_set_size(vig, disc, disc);
        lv_obj_align(vig, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(vig, lv_color_hex(theme.panel), 0);
        lv_obj_set_style_bg_opa(vig, LV_OPA_80, 0);
        lv_obj_set_style_radius(vig, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(vig, 0, 0);
        lv_obj_set_style_pad_all(vig, 0, 0);
        lv_obj_clear_flag(vig, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(vig, LV_OBJ_FLAG_CLICKABLE);
    }

    t.value = lv_label_create(ring);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // CTS line tracks the ring's bottom (label-tracks-number geometry).
    t.secondary = lv_label_create(t.root);
    lv_label_set_text(t.secondary, "");
    lv_obj_set_style_text_font(t.secondary, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.warn), 0);
    lv_obj_align_to(t.secondary, ring, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);
}

// Gauge widget: LVGL arc spanning 270° with the value fill in accent
// and a center percent label. Mirrors editor .wpreview .gauge.
//
// Tick scale: a ranged element (MIDL format.range, i.e. range_min != range_max)
// drives the surrounding lv_scale with the binding's real [min,max] and turns on
// numeric labels, so e.g. a rudder gauge on [-35,35] shows -35 … 0 … 35 around
// the arc. The arc *fill* keeps its 0..100 internal range (the update path feeds
// it a percent via binding_unit_fraction), so only the decorative tick ring
// follows the real scale. Legacy/default tiles (range_min==range_max) keep the
// label-less 0–100 tick ring byte-for-byte.
static void paint_gauge_body(QuadGridTile &t, const MetricBinding &m, int w, int h) {
    const bool ranged = (m.range_min != m.range_max);
    int dia = (w < h ? w : h) - 56;
    if (dia < 88) dia = 88;
    lv_obj_t *arc = lv_arc_create(t.root);
    lv_obj_set_size(arc, dia, dia);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 4);
    lv_arc_set_bg_angles(arc, 135, 45);  // 270 degree sweep, bottom open
    lv_arc_set_range(arc, 0, 100);       // fill is percent-driven; see header note
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(theme.grid), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(theme.accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    t.aux = arc;

    // Tick ring (LVGL 9 lv_scale). Default tiles: 5 unlabeled ticks over 0–100,
    // matching the editor preview. Ranged tiles: span the binding's real
    // [range_min,range_max] and show numeric labels at the 5 major ticks (rounded
    // to whole units) so the operator reads the true scale endpoints.
    lv_obj_t *scale = lv_scale_create(t.root);
    lv_obj_set_size(scale, dia, dia);
    lv_obj_align(scale, LV_ALIGN_CENTER, 0, 4);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_angle_range(scale, 270);
    lv_scale_set_rotation(scale, 135);
    lv_scale_set_total_tick_count(scale, 5);
    lv_scale_set_major_tick_every(scale, 1);
    if (ranged) {
        // lv_scale range is integer; round the binding's float window to the
        // nearest whole unit. lo<hi is guaranteed by ranged && the painter only
        // labels endpoints, so an inverted/degenerate pair just shows lo..lo.
        int lo = (int)lroundf(m.range_min);
        int hi = (int)lroundf(m.range_max);
        if (hi <= lo) hi = lo + 1;  // defensive: keep a valid increasing range
        lv_scale_set_range(scale, lo, hi);
        lv_scale_set_label_show(scale, true);
        lv_obj_set_style_text_font(scale, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(scale, lv_color_hex(theme.fg_dim), LV_PART_MAIN);
    } else {
        lv_scale_set_range(scale, 0, 100);
    }
    lv_obj_set_style_length(scale, 6, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(theme.fg_dim), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scale, LV_OBJ_FLAG_SCROLLABLE);

    // Percent label is the gauge's hero number, sized to dominate the
    // arc's open bottom. NO second caption beneath - the chrome already
    // carries the metric label in the tile's top-left (removed the
    // duplicate `cap` label that overlapped the percent on small tiles).
    const lv_font_t *vfont = &lv_font_montserrat_28;
    if (dia >= 110) vfont = &lv_font_montserrat_38;
    if (dia >= 160) vfont = &lv_font_montserrat_48;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 4);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
}

// Bar widget: title row (label left, percent right) + LVGL bar fill.
// Mirrors editor .wpreview .bar.
static void paint_bar_body(QuadGridTile &t, const MetricBinding & /*m*/, int w, int h) {
    // Bar tile: percent number is the hero element (centered, big), bar
    // sits below it. Percent dominates the tile so the operator can
    // read SOC / fuel / fresh-water at a glance, like editor's preview.
    int bar_h = h < 130 ? 18 : 22;
    int bar_w = w - 32;

    const lv_font_t *vfont = &lv_font_montserrat_38;
    if (h < 140) vfont = &lv_font_montserrat_28;
    if (h >= 180) vfont = &lv_font_montserrat_48;

    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--%");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, -18);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bar = lv_bar_create(t.root);
    lv_obj_set_size(bar, bar_w, bar_h);
    // Bar sits in the lower third, tracking under the percent number.
    lv_obj_align_to(bar, t.value, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x001a20), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(theme.panel_edge), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme.good), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    t.aux = bar;
}

// Trend widget: current reading (hero number) above a rolling sparkline. The
// sparkline is a tile-sized lv_chart LINE series with no axes/markers — distinct
// from the fullscreen TrendChartState used as a standalone screen template.
//
// History is the chart's own point ring: update pushes one normalized sample
// (0..100 via binding_unit_fraction, the same scale as the gauge/bar fill) with
// lv_chart_set_next_value, so no separate per-tile sample buffer is needed. A
// ranged element (format.range) normalizes against its [min,max]; legacy tiles
// use the per-source heuristic. Samples are pushed on value-change only, matching
// the fullscreen trend (a steady reading holds the line rather than scrolling it).
static void paint_trend_body(QuadGridTile &t, const MetricBinding &m, int w, int h) {
    // Current reading: hero number, upper third of the tile.
    const lv_font_t *vfont = &lv_font_montserrat_28;
    if (h >= 160) vfont = &lv_font_montserrat_38;
    if (w < 140) vfont = &lv_font_montserrat_20;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_align(t.value, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // Sparkline fills the lower half of the tile.
    int chart_w = w - 24;
    int chart_h = h / 2 - 12;
    if (chart_h < 36) chart_h = 36;
    lv_obj_t *chart = lv_chart_create(t.root);
    lv_obj_set_size(chart, chart_w, chart_h);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 30);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    // Hide the per-point dot markers: a clean line is the sparkline idiom.
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t accent = m.accent ? m.accent : theme.accent;
    lv_chart_series_t *s =
        lv_chart_add_series(chart, lv_color_hex(accent), LV_CHART_AXIS_PRIMARY_Y);
    // Start empty so the line grows in from the right instead of sitting at 0.
    lv_chart_set_all_values(chart, s, LV_CHART_POINT_NONE);
    t.aux = chart;
    t.last_aux_pct = -1;  // unset; first real sample always pushes
}

// Wind rose: dashed warn ring with center AWS value. Mirrors editor
// .wpreview .rose - small visual stand-in; the fullscreen wind dial
// (screen_wind.cpp) is the high-fidelity render.
static void paint_wind_rose_body(QuadGridTile &t, const MetricBinding &m, int w, int h) {
    int dia = (w < h ? w : h) - 56;
    if (dia < 80) dia = 80;
    lv_obj_t *ring = lv_obj_create(t.root);
    lv_obj_set_size(ring, dia, dia);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(theme.warn), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    t.aux = ring;

    // Apparent + true wind heads orbiting OUTSIDE the ring (bow-up, bow-relative),
    // so they never overlap the centre AWS number. Apparent = filled amber chevron
    // pointing in; true = hollow green chevron pointing out. Bearings filled live
    // in update from metric_scalar(); reference is the bow (0).
    ui::MarkerSpec wind_markers[2] = {
        {NAN, ui::Glyph::ChevronIn, true, theme.warn},    // apparent wind
        {NAN, ui::Glyph::ChevronOut, false, theme.good},  // true wind
    };
    int mcx, mcy;
    dial_center(w, h, mcx, mcy);
    t.markers = ui::build_marker_ring(t.root, mcx, mcy, dia / 2, wind_markers, 2,
                                      /*occlude_lower=*/false);

    // Wind speed (AWS) is the hero number, sized to dominate the ring (the
    // marker is now outside, so the number can fill the dial).
    const lv_font_t *vfont = &lv_font_montserrat_38;
    if (dia >= 110) vfont = &lv_font_montserrat_48;
    if (dia >= 135) vfont = &font_xl_64;
    if (dia < 90) vfont = &lv_font_montserrat_28;
    t.value = lv_label_create(ring);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.warn), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, m.unit && m.unit[0] ? -8 : 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // Unit ("kn") under the AWS hero number so the wind dial is consistent with
    // the SOG / DEPTH numeric tiles (was a bare number with no unit).
    if (m.unit && m.unit[0]) {
        t.unit = lv_label_create(ring);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align_to(t.unit, t.value, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Text widget: monospace value centered. Mirrors editor .wpreview .text-val.
static void paint_text_body(QuadGridTile &t, const MetricBinding & /*m*/, int /*w*/, int h) {
    // Text widget covers multi-line values like position (lat / lon on
    // two lines). Pick a font that won't overflow short tiles.
    const lv_font_t *vfont = &lv_font_montserrat_20;
    if (h >= 160) vfont = &lv_font_montserrat_28;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_text_align(t.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 6);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
}

// Button widget: rounded accent bubble. Mirrors editor .wpreview .btn-bubble.
static void paint_button_body(QuadGridTile &t, const MetricBinding &m, int /*w*/, int /*h*/) {
    lv_obj_t *btn = lv_obj_create(t.root);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_pad_hor(btn, 18, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    // Not itself clickable: the action handler lives on the tile root (build_tile),
    // so the indev hit-test must walk past this bubble to the root. Without this,
    // the bubble would swallow the tap and the action would never fire.
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, (m.label && m.label[0]) ? m.label : "TAP");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x001218), 0);
    t.aux = btn;
}

// Autopilot widget: state pill (green) + target text + 4 nudge buttons row.
// Mirrors editor .wpreview .ap.
static void paint_autopilot_body(QuadGridTile &t, const MetricBinding & /*m*/, int w, int /*h*/) {
    // State pill.
    lv_obj_t *pill = lv_obj_create(t.root);
    lv_obj_set_size(pill, w - 32, 26);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x143b2a), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(theme.good), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_radius(pill, 4, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    t.aux2 = pill;

    t.value = lv_label_create(pill);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(theme.good), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // Target text below the pill.
    t.secondary = lv_label_create(t.root);
    lv_label_set_text(t.secondary, "target ---");
    lv_obj_set_style_text_font(t.secondary, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align_to(t.secondary, pill, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);

    // 4 nudge buttons (-10/-1/+1/+10) row across the bottom.
    int btn_w = (w - 40) / 4;
    static const char *labels[] = {"-10", "-1", "+1", "+10"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b = lv_obj_create(t.root);
        lv_obj_set_size(b, btn_w, 22);
        // Single align call on `b` (not t.root - that was a typo that
        // repositioned the entire tile 4x per build, causing the tile
        // to drift to bottom-left).
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 16 + i * (btn_w + 2), -8);
        lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel_edge), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, labels[i]);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(theme.fg), 0);
        lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    }
}

static QuadGridTile build_tile(lv_obj_t *parent, int x, int y, int w, int h,
                               const MetricBinding &m) {
    QuadGridTile t = {};
    t.idx = -1;
    t.kind = m.kind;
    t.last_aux_pct = -1;
    strncpy(t.last_value, "\xFF", sizeof(t.last_value));
    strncpy(t.last_secondary, "\xFF", sizeof(t.last_secondary));
    for (int i = 0; i < 4; ++i)
        strncpy(t.last_extras[i], "\xFF", sizeof(t.last_extras[0]));

    // --- Chrome: glass-cockpit panel (gradient + border via style_panel) ---
    // No left accent rail (dropped per design feedback); semantic color lives
    // in the value text instead. style_panel applies the consolidated tokens.
    t.root = lv_obj_create(parent);
    lv_obj_set_size(t.root, w, h);
    lv_obj_set_pos(t.root, x, y);
    style_panel(t.root);

    t.cap = lv_label_create(t.root);
    lv_label_set_text(t.cap, m.label ? m.label : "");
    lv_obj_set_style_text_font(t.cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t.cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_pos(t.cap, 10, 2);
    lv_obj_clear_flag(t.cap, LV_OBJ_FLAG_CLICKABLE);

    // --- Body: dispatched per widget kind ---
    switch (m.kind) {
    case WidgetKind::Compass:
        paint_compass_body(t, m, w, h);
        break;
    case WidgetKind::Gauge:
        paint_gauge_body(t, m, w, h);
        break;
    case WidgetKind::Bar:
        paint_bar_body(t, m, w, h);
        break;
    case WidgetKind::WindRose:
        paint_wind_rose_body(t, m, w, h);
        break;
    case WidgetKind::Text:
        paint_text_body(t, m, w, h);
        break;
    case WidgetKind::Button:
        paint_button_body(t, m, w, h);
        break;
    case WidgetKind::Autopilot:
        paint_autopilot_body(t, m, w, h);
        break;
    case WidgetKind::Trend:
        paint_trend_body(t, m, w, h);
        break;
    case WidgetKind::Numeric:
    default:
        paint_numeric_body(t, m, w, h);
        break;
    }

    // Button elements with an action (nav or command) make the WHOLE tile the
    // tap target via button_action_cb. The inner bubble has CLICKABLE cleared in
    // paint_button_body so the hit-test walks up to the tile root. Checked before
    // the generic target_screen nav so a nav-button still routes through the
    // button handler (identical effect, but keeps the binding-ptr user_data).
    bool button_action = (m.kind == WidgetKind::Button) &&
                         ((m.command && m.command[0]) || (m.target_screen && m.target_screen[0]));
    if (button_action) {
        lv_obj_add_flag(t.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t.root, button_action_cb, LV_EVENT_CLICKED, (void *)&m);
    } else if (m.target_screen && m.target_screen[0]) {
        lv_obj_add_event_cb(t.root, tile_clicked_cb, LV_EVENT_CLICKED, (void *)m.target_screen);
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

    QuadGridState *st =
        (QuadGridState *)heap_caps_calloc(1, sizeof(QuadGridState), MALLOC_CAP_INTERNAL);
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
        // Tap-to-zoom (Slice 6): per-field dispatch. A legacy/hardcoded tile
        // (zoom_target == NULL) with a real metric opens the full-screen "zoom"
        // view; an authored tile follows its resolved zoom_action (auto-scale
        // or switch to a referenced screen). user_data points into the
        // persistent spec.metrics[] so the binding outlives the tap.
        if (i < spec.metric_count) {
            const MetricBinding &mb = spec.metrics[i];
            bool interactive =
                mb.zoom_target
                    ? (layout::zoom_action(mb.zoomable, mb.zoom_target) != layout::ZOOM_NONE)
                    : (mb.source != MetricSource::None);
            if (interactive) {
                lv_obj_add_flag(st->tiles[i].root, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(st->tiles[i].root, tile_zoom_action_cb, LV_EVENT_CLICKED,
                                    (void *)&spec.metrics[i]);
            }
        }
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_quad_grid(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    if (!root) return;
    auto *st = (QuadGridState *)lv_obj_get_user_data(root);
    if (!st) return;
    for (int i = 0; i < 4; ++i) {
        QuadGridTile &t = st->tiles[i];
        if (t.idx < 0 || t.idx >= spec.metric_count) continue;
        const MetricBinding &m = spec.metrics[t.idx];

        char pri[40], sec[24];
        format_metric(m, data, pri, sizeof(pri), sec, sizeof(sec));

        // Per-kind aux updates (gauge arc fill, bar fill).
        switch (t.kind) {
        case WidgetKind::Gauge:
        case WidgetKind::Bar: {
            double scalar = metric_scalar(m, data);
            // Honor an explicit MIDL format.range when present; legacy bindings
            // (range_min==range_max) fall back to the built-in per-source heuristic.
            double frac = binding_unit_fraction(m, scalar);
            int pct = isnan(frac) ? 0 : (int)(frac * 100.0 + 0.5);
            if (t.aux && pct != t.last_aux_pct) {
                if (t.kind == WidgetKind::Gauge)
                    lv_arc_set_value(t.aux, pct);
                else
                    lv_bar_set_value(t.aux, pct, LV_ANIM_OFF);
                t.last_aux_pct = pct;
            }
            // Center label. Legacy tiles (no explicit MIDL format.range) show the
            // percent of the heuristic range, matching the editor preview. When the
            // element carries an explicit format.range, show the actual scalar value
            // (formatted to format.precision, or 0 dp by default) so a real gauge —
            // e.g. rudder on [-35,35] — reads its true magnitude, not a percent.
            char buf[24];
            if (m.range_min != m.range_max) {
                if (isnan(scalar))
                    snprintf(buf, sizeof(buf), "--");
                else {
                    int dp = m.precision >= 0 ? m.precision : 0;
                    snprintf(buf, sizeof(buf), "%.*f", dp, scalar);
                }
            } else if (isnan(frac)) {
                snprintf(buf, sizeof(buf), "--");
            } else {
                snprintf(buf, sizeof(buf), "%d%%", pct);
            }
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), buf);
            break;
        }
        case WidgetKind::Autopilot:
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.secondary) {
                char tgt[24];
                snprintf(tgt, sizeof(tgt), "target %s", sec[0] ? sec : "---");
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        tgt);
            }
            break;
        case WidgetKind::Button:
            // Static label; no update needed.
            break;
        case WidgetKind::Compass:
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.secondary) {
                // Compass CTS line: prefer extras[0] (operator-bound second
                // path like CTS or COG); fall back to format_metric's secondary.
                char cts[24];
                if (m.extras_count > 0 && m.extras[0].source != MetricSource::None) {
                    MetricBinding eb = {};
                    eb.source = m.extras[0].source;
                    char ep[24], esec[24];
                    format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
                    snprintf(cts, sizeof(cts), "%s %s",
                             m.extras[0].label && m.extras[0].label[0] ? m.extras[0].label : "CTS",
                             ep);
                } else {
                    snprintf(cts, sizeof(cts), "%s", sec);
                }
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        cts);
            }
            // Markers: HDG/COG/CTS bearings at their true (north-up) bearings.
            {
                MetricBinding hdg = {}, cog = {}, cts = {};
                hdg.source = MetricSource::HDG_deg;
                cog.source = MetricSource::COG_deg;
                cts.source = MetricSource::CTS_deg;
                ui::MarkerSpec live[3] = {
                    {metric_scalar(hdg, data), ui::Glyph::Triangle, true, theme.accent},
                    {metric_scalar(cog, data), ui::Glyph::Triangle, false, theme.good},
                    {metric_scalar(cts, data), ui::Glyph::Diamond, true, theme.alarm},
                };
                // Fixed-bezel north-up tile: markers sit at their true bearings (HDG/COG/CTS),
                // matching the static N/E/S/W cardinals. (The AP HUD, whose scale rotates, is
                // heading-up; this round tile is not.)
                ui::marker_ring_update(t.markers, live, 3, /*reference=*/0.0);
            }
            break;
        case WidgetKind::Trend: {
            // Hero number is the live reading; sparkline gets one normalized
            // sample (0..100, same scale as the gauge/bar fill) on value-change.
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.aux) {
                double frac = binding_unit_fraction(m, metric_scalar(m, data));
                if (!isnan(frac)) {
                    int v = (int)(frac * 100.0 + 0.5);
                    if (v != t.last_aux_pct) {
                        lv_chart_series_t *s = lv_chart_get_series_next(t.aux, NULL);
                        if (s) {
                            lv_chart_set_next_value(t.aux, s, v);
                            lv_chart_refresh(t.aux);
                        }
                        t.last_aux_pct = v;
                    }
                }
            }
            break;
        }
        default:
            // Numeric, WindRose, Text fallback - all use
            // the value/secondary text slots populated by format_metric.
            if (ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri) &&
                t.kind == WidgetKind::Numeric && m.extras_count == 0) {
                // Shrink the hero value to fit the tile (scaled k/M strings + a
                // P/S suffix can be wider than the big font allows).
                fit_value_font(t.value, pri, QG_TILE_W - 56);
            }
            // VMG sign tint: red when negative (opening from the mark / losing
            // ground), green when making good toward it. Neutral on NaN so a
            // dropout doesn't flash a false good/bad cue. Applied on the value
            // label only for the VMG source.
            if (t.value && m.source == MetricSource::VMG_kn) {
                double vmg = mps_to_kn(data.vmg);
                uint32_t col = isnan(vmg) ? theme.fg : (vmg < 0 ? theme.alarm : theme.good);
                lv_obj_set_style_text_color(t.value, lv_color_hex(col), 0);
            }
            if (t.secondary) {
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        sec);
            }
            // WindRose: apparent + true wind heads, bow-up and bow-relative
            // (AWA/TWA are angles relative to the bow, 0 = wind from ahead).
            if (t.kind == WidgetKind::WindRose) {
                MetricBinding awa = {}, twa = {};
                awa.source = MetricSource::AWA_deg;
                twa.source = MetricSource::TWA_deg;
                ui::MarkerSpec wind[2] = {
                    {metric_scalar(awa, data), ui::Glyph::ChevronIn, true, theme.warn},
                    {metric_scalar(twa, data), ui::Glyph::ChevronOut, false, theme.good},
                };
                ui::marker_ring_update(t.markers, wind, 2, /*reference=*/0.0);  // bow-up rose
            }
            break;
        }

        // Multi-row extras (only meaningful for Numeric kind).
        for (uint8_t e = 0; e < m.extras_count && e < 4; ++e) {
            if (!t.extras[e]) continue;
            MetricBinding eb = {};
            eb.source = m.extras[e].source;
            char ep[24], esec[24];
            format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
            char row[32];
            if (m.extras[e].label && m.extras[e].label[0])
                snprintf(row, sizeof(row), "%s %s", m.extras[e].label, ep);
            else
                snprintf(row, sizeof(row), "%s", ep);
            ui::set_text_if_changed(t.extras[e], t.last_extras[e], sizeof(t.last_extras[e]), row);
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

    HeroPlusState *st =
        (HeroPlusState *)heap_caps_calloc(1, sizeof(HeroPlusState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] hero_plus alloc failed");
        return root;
    }
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

    // Hero caption: the binding label (e.g. "BELOW KEEL") so the hero datum is
    // unambiguous even when the screen title is generic ("Depth"). Distinct
    // from below-transducer, which is otherwise indistinguishable.
    if (st->metric.label && st->metric.label[0]) {
        lv_obj_t *hcap = lv_label_create(hero);
        lv_label_set_text(hcap, st->metric.label);
        lv_obj_set_style_text_font(hcap, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hcap, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(hcap, LV_ALIGN_TOP_LEFT, 12, 10);
    }

    st->primary_value = lv_label_create(hero);
    lv_label_set_text(st->primary_value, "--");
    lv_obj_set_style_text_font(st->primary_value, &lv_font_montserrat_48, 0);
    // Accent color matches editor preview's .num-primary.
    lv_obj_set_style_text_color(st->primary_value, lv_color_hex(theme.accent), 0);
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

static void update_hero_plus(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (HeroPlusState *)lv_obj_get_user_data(root);
    if (!st) return;

    char pri[40], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->primary_value, st->last_primary, sizeof(st->last_primary), pri);
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
        ui::set_text_if_changed(st->extras_value[i], st->last_extras[i], sizeof(st->last_extras[i]),
                                ep);
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

    StatusListState *st =
        (StatusListState *)heap_caps_calloc(1, sizeof(StatusListState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] status_list alloc failed");
        return root;
    }
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
        char pri[40], sec[24];
        format_metric(spec.metrics[i], data, pri, sizeof(pri), sec, sizeof(sec));
        char combined[32];
        if (spec.metrics[i].unit && spec.metrics[i].unit[0]) {
            snprintf(combined, sizeof(combined), "%s %s", pri, spec.metrics[i].unit);
        } else {
            snprintf(combined, sizeof(combined), "%s", pri);
        }
        ui::set_text_if_changed(st->value_labels[i], st->last_values[i], sizeof(st->last_values[i]),
                                combined);
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

static lv_obj_t *create_round_instrument(lv_obj_t *parent, const ScreenVariantSpec &spec) {
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
    if (!st) {
        net::logf("[layout] round_inst alloc failed");
        return root;
    }
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
    char pri[40], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->value, st->last_value, sizeof(st->last_value), pri);
    if (st->secondary) {
        ui::set_text_if_changed(st->secondary, st->last_secondary, sizeof(st->last_secondary), sec);
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

static void split_half_build(lv_obj_t *parent, int x, int y, int w, int h, const MetricBinding &m,
                             SplitHalf &out) {
    out.metric = m;
    strncpy(out.last_value, "\xFF", sizeof(out.last_value));
    strncpy(out.last_secondary, "\xFF", sizeof(out.last_secondary));

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, x, y);
    // Glass-cockpit chrome via the consolidated style; this hero variant
    // centre-aligns its content, so override padding back to 0. No accent rail.
    style_panel(panel);
    lv_obj_set_style_pad_all(panel, 0, 0);

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

static lv_obj_t *create_split_pair(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    SplitPairState *st =
        (SplitPairState *)heap_caps_calloc(1, sizeof(SplitPairState), MALLOC_CAP_INTERNAL);
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

    split_half_build(root, left_x, top_y, half_w, h, spec.metrics[0], st->left);
    if (spec.metric_count >= 2) {
        split_half_build(root, right_x, top_y, half_w, h, spec.metrics[1], st->right);
    } else {
        // single-metric mode: stretch left to full width
        lv_obj_set_size(lv_obj_get_child(root, 1), LCD_W - 16, h);
    }

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_split_pair(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    if (!root) return;
    auto *st = (SplitPairState *)lv_obj_get_user_data(root);
    if (!st) return;

    auto update_half = [&](SplitHalf &h) {
        if (!h.value) return;
        char pri[40], sec[24];
        format_metric(h.metric, data, pri, sizeof(pri), sec, sizeof(sec));
        ui::set_text_if_changed(h.value, h.last_value, sizeof(h.last_value), pri);
        if (h.secondary) {
            ui::set_text_if_changed(h.secondary, h.last_secondary, sizeof(h.last_secondary), sec);
        }
    };
    update_half(st->left);
    if (spec.metric_count >= 2) update_half(st->right);
}

// ---------------------------------------------------------------------------
// trend_chart template - large primary value + rolling N-sample chart.
// Auto-scales the Y axis to the running min/max of the visible window
// so the trace stays useful across very different metrics (depth 0.5..40
// vs battery 11..14 vs SOG 0..10).
//
// Single MetricBinding[0]. If metric.unit is set, shown next to value.
// Skips inserting a new sample when the scalar hasn't moved by more
// than 1 % of the visible range (keeps the trace from drawing a flat
// line of N identical points and forcing redraws).

struct TrendChartState {
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *secondary;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    char last_value[24];
    char last_secondary[24];
    MetricBinding metric;
    static constexpr int POINTS = 60;
    double samples[POINTS];
    int next_idx = 0;
    int filled = 0;
    double last_sample = NAN;
};

static lv_obj_t *create_trend_chart(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    TrendChartState *st =
        (TrendChartState *)heap_caps_calloc(1, sizeof(TrendChartState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] trend_chart alloc failed");
        return root;
    }
    st->metric = spec.metrics[0];
    strncpy(st->last_value, "\xFF", sizeof(st->last_value));
    strncpy(st->last_secondary, "\xFF", sizeof(st->last_secondary));

    // Title (caption) top-left.
    lv_obj_t *cap = lv_label_create(root);
    lv_label_set_text(cap, st->metric.label ? st->metric.label : "");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 16, 16);

    st->value = lv_label_create(root);
    lv_label_set_text(st->value, "--");
    lv_obj_set_style_text_font(st->value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->value, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->value, LV_ALIGN_TOP_LEFT, 16, 40);

    if (st->metric.unit && st->metric.unit[0]) {
        st->unit = lv_label_create(root);
        lv_label_set_text(st->unit, st->metric.unit);
        lv_obj_set_style_text_font(st->unit, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(st->unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(st->unit, LV_ALIGN_TOP_LEFT, 180, 56);
    }

    st->secondary = lv_label_create(root);
    lv_label_set_text(st->secondary, "");
    lv_obj_set_style_text_font(st->secondary, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->secondary, LV_ALIGN_TOP_RIGHT, -16, 16);

    // Chart fills the lower 240 px.
    st->chart = lv_chart_create(root);
    lv_obj_set_size(st->chart, LCD_W - 32, 240);
    lv_obj_align(st->chart, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_chart_set_type(st->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(st->chart, TrendChartState::POINTS);
    lv_chart_set_div_line_count(st->chart, 4, 6);
    lv_chart_set_range(st->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_color(st->chart, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(st->chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(st->chart, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(st->chart, 1, 0);
    lv_obj_set_style_radius(st->chart, 8, 0);
    lv_obj_set_style_line_color(st->chart, lv_color_hex(theme.grid), LV_PART_MAIN);
    lv_obj_set_style_line_color(st->chart, lv_color_hex(theme.grid), LV_PART_ITEMS);
    st->series =
        lv_chart_add_series(st->chart, lv_color_hex(st->metric.accent), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < TrendChartState::POINTS; ++i) {
        st->samples[i] = NAN;
    }
    lv_obj_set_user_data(root, st);
    return root;
}

static void update_trend_chart(lv_obj_t *root, const ScreenVariantSpec &spec,
                               const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (TrendChartState *)lv_obj_get_user_data(root);
    if (!st) return;

    // Text update (primary + secondary) always.
    char pri[40], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->value, st->last_value, sizeof(st->last_value), pri);
    if (st->secondary) {
        ui::set_text_if_changed(st->secondary, st->last_secondary, sizeof(st->last_secondary), sec);
    }

    // Insert a new sample only when the value changed.
    double v = metric_scalar(st->metric, data);
    if (isnan(v)) return;
    if (!isnan(st->last_sample) && fabs(v - st->last_sample) < 1e-3) return;
    st->last_sample = v;
    st->samples[st->next_idx] = v;
    st->next_idx = (st->next_idx + 1) % TrendChartState::POINTS;
    if (st->filled < TrendChartState::POINTS) st->filled++;

    // Recompute min/max over the filled window.
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < st->filled; ++i) {
        double s = st->samples[i];
        if (isnan(s)) continue;
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    if (!isfinite(lo) || !isfinite(hi)) return;
    if (fabs(hi - lo) < 1e-3) {
        hi += 0.5;
        lo -= 0.5;
    }
    // Pad 10 % so the trace doesn't kiss the bezel.
    double pad = (hi - lo) * 0.1;
    lo -= pad;
    hi += pad;
    // lv_chart works in integer coords; scale to fixed-point x10.
    int32_t y_min = (int32_t)(lo * 10);
    int32_t y_max = (int32_t)(hi * 10);
    if (y_min == y_max) y_max = y_min + 1;
    lv_chart_set_range(st->chart, LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);

    // Walk the rolling buffer chronologically into the chart series.
    int p = (st->next_idx + TrendChartState::POINTS - st->filled) % TrendChartState::POINTS;
    for (int i = 0; i < TrendChartState::POINTS; ++i) {
        if (i < TrendChartState::POINTS - st->filled) {
            lv_chart_set_value_by_id(st->chart, st->series, i, LV_CHART_POINT_NONE);
        } else {
            double s = st->samples[p];
            p = (p + 1) % TrendChartState::POINTS;
            if (isnan(s)) {
                lv_chart_set_value_by_id(st->chart, st->series, i, LV_CHART_POINT_NONE);
            } else {
                lv_chart_set_value_by_id(st->chart, st->series, i, (int32_t)(s * 10));
            }
        }
    }
    lv_chart_refresh(st->chart);
}

// ---------------------------------------------------------------------------
// alert_focus template - single-value full-screen view that flips between
// nominal / warning / alarm visual states when the metric crosses a
// threshold.
//
// Thresholds come from the existing runtime alarm config
// (ui::depth_alarm_m, ui::battery_alarm_v) so screen authors don't
// duplicate them. For metrics without an alarm config, the template
// stays in the nominal state.
//
// Visual states:
//   nominal: theme.panel bg, accent rail
//   alarm  : theme.alarm bg, white text, banner-style title
//
// Use as a focus screen for safety overlays - the existing
// `alarm_banner` global pill still fires on top; this template gives
// a dedicated tap-target screen that focuses one metric at a time.

struct AlertFocusState {
    lv_obj_t *root_panel;
    lv_obj_t *cap;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *status;
    char last_value[24];
    char last_status[24];
    MetricBinding metric;
    bool in_alarm = false;
};

static bool alarm_for(const MetricBinding &m, const sk::Data &d) {
    switch (m.source) {
    case MetricSource::Depth_m:
        return !isnan(d.depth) && d.depth > 0 && d.depth < ui::depth_alarm_m();
    case MetricSource::BatteryV:
        return !isnan(d.battVoltage) && d.battVoltage < ui::battery_alarm_v();
    default:
        return false;
    }
}

static lv_obj_t *create_alert_focus(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    if (spec.metric_count < 1) return nullptr;
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    AlertFocusState *st =
        (AlertFocusState *)heap_caps_calloc(1, sizeof(AlertFocusState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] alert_focus alloc failed");
        return root;
    }
    st->metric = spec.metrics[0];
    strncpy(st->last_value, "\xFF", sizeof(st->last_value));
    strncpy(st->last_status, "\xFF", sizeof(st->last_status));

    st->root_panel = lv_obj_create(root);
    lv_obj_set_size(st->root_panel, LCD_W - 16, LCD_H - 16 - 64);
    lv_obj_set_pos(st->root_panel, 8, 64);
    lv_obj_set_style_bg_color(st->root_panel, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(st->root_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(st->root_panel, lv_color_hex(st->metric.accent), 0);
    lv_obj_set_style_border_width(st->root_panel, 4, 0);
    lv_obj_set_style_radius(st->root_panel, 12, 0);
    lv_obj_set_style_pad_all(st->root_panel, 0, 0);
    lv_obj_clear_flag(st->root_panel, LV_OBJ_FLAG_SCROLLABLE);

    st->cap = lv_label_create(st->root_panel);
    lv_label_set_text(st->cap, st->metric.label ? st->metric.label : "");
    lv_obj_set_style_text_font(st->cap, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->cap, LV_ALIGN_TOP_MID, 0, 20);

    st->value = lv_label_create(st->root_panel);
    lv_label_set_text(st->value, "--");
    // Largest available font; spec 12 visual adoption notes go further
    // when a 60-72 pt font ships.
    lv_obj_set_style_text_font(st->value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->value, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->value, LV_ALIGN_CENTER, 0, -10);

    if (st->metric.unit && st->metric.unit[0]) {
        st->unit = lv_label_create(st->root_panel);
        lv_label_set_text(st->unit, st->metric.unit);
        lv_obj_set_style_text_font(st->unit, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(st->unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align(st->unit, LV_ALIGN_CENTER, 0, 40);
    }

    st->status = lv_label_create(st->root_panel);
    lv_label_set_text(st->status, "");
    lv_obj_set_style_text_font(st->status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(st->status, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->status, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_alert_focus(lv_obj_t *root, const ScreenVariantSpec &spec,
                               const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (AlertFocusState *)lv_obj_get_user_data(root);
    if (!st) return;

    char pri[40], sec[24];
    format_metric(st->metric, data, pri, sizeof(pri), sec, sizeof(sec));
    ui::set_text_if_changed(st->value, st->last_value, sizeof(st->last_value), pri);

    bool now_alarm = alarm_for(st->metric, data);
    const char *status_text = now_alarm ? "ALARM" : (sec[0] ? sec : "nominal");
    ui::set_text_if_changed(st->status, st->last_status, sizeof(st->last_status), status_text);

    if (now_alarm != st->in_alarm) {
        st->in_alarm = now_alarm;
        uint32_t bg = now_alarm ? theme.alarm : theme.panel;
        uint32_t fg = now_alarm ? 0xffffff : theme.fg;
        uint32_t fg_dim = now_alarm ? 0xffe5e5 : theme.fg_dim;
        lv_obj_set_style_bg_color(st->root_panel, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(st->root_panel,
                                      lv_color_hex(now_alarm ? theme.alarm : st->metric.accent), 0);
        lv_obj_set_style_text_color(st->value, lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(st->cap, lv_color_hex(fg_dim), 0);
        if (st->unit) lv_obj_set_style_text_color(st->unit, lv_color_hex(fg_dim), 0);
        lv_obj_set_style_text_color(st->status, lv_color_hex(fg_dim), 0);
    }
}

// ---------------------------------------------------------------------------
// control_console template - autopilot-specific mode + adjust panel.
//
// Layout:
//   top row    : mode segmented buttons (Standby / Auto / Wind / Track)
//   middle     : large current/target heading display (deg, montserrat_48)
//   bottom row : -10 | -1 | +1 | +10 heading-delta buttons + Silence
//
// Click handlers call autopilot:: which posts to the net worker queue,
// so this runs safely from the LVGL task. State is read on each
// update() call so the active-mode highlight reflects what the
// emulator/backend actually believes (not just what we last asked for).

struct ControlConsoleState {
    lv_obj_t *mode_btns[5];  // standby auto wind pretrack track
    autopilot::Mode mode_for[5];
    lv_obj_t *current_lbl;
    lv_obj_t *target_lbl;
    lv_obj_t *backend_lbl;
    autopilot::Mode last_mode = autopilot::Mode::Unknown;
    char last_current[16];
    char last_target[16];
};

static void cc_mode_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto mode = static_cast<autopilot::Mode>((uintptr_t)lv_event_get_user_data(e));
    autopilot::Result r = autopilot::set_mode(mode);
    net::logf("[cc] set_mode(%s) -> %d", autopilot::mode_name(mode), (int)r);
}

static void cc_adjust_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    autopilot::Result r = autopilot::adjust_heading_deg(delta);
    net::logf("[cc] adjust(%+d) -> %d", delta, (int)r);
}

static void cc_silence_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    autopilot::Result r = autopilot::silence_alarm();
    net::logf("[cc] silence -> %d", (int)r);
}

static lv_obj_t *cc_make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label,
                                uint32_t bg, uint32_t fg, lv_event_cb_t cb, void *user) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static lv_obj_t *create_control_console(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    ControlConsoleState *st = (ControlConsoleState *)heap_caps_calloc(
        1, sizeof(ControlConsoleState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] control_console alloc failed");
        return root;
    }
    strncpy(st->last_current, "\xFF", sizeof(st->last_current));
    strncpy(st->last_target, "\xFF", sizeof(st->last_target));

    // Title.
    if (spec.title && spec.title[0]) {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, spec.title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    }

    // Mode row (4 buttons across) at y=70..130.
    const char *names[4] = {"STANDBY", "AUTO", "WIND", "TRACK"};
    autopilot::Mode modes[4] = {
        autopilot::Mode::Standby,
        autopilot::Mode::Auto,
        autopilot::Mode::Wind,
        autopilot::Mode::Track,
    };
    int mode_w = (LCD_W - 16 - 12) / 4;  // 12px gaps
    for (int i = 0; i < 4; ++i) {
        int x = 8 + i * (mode_w + 4);
        st->mode_btns[i] = cc_make_button(root, x, 70, mode_w, 60, names[i], theme.panel, theme.fg,
                                          cc_mode_btn_cb, (void *)(uintptr_t)modes[i]);
        st->mode_for[i] = modes[i];
    }

    // Current heading + target.
    lv_obj_t *cap_cur = lv_label_create(root);
    lv_label_set_text(cap_cur, "CURRENT");
    lv_obj_set_style_text_font(cap_cur, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap_cur, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap_cur, LV_ALIGN_TOP_LEFT, 24, 152);

    st->current_lbl = lv_label_create(root);
    lv_label_set_text(st->current_lbl, "---");
    lv_obj_set_style_text_font(st->current_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->current_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->current_lbl, LV_ALIGN_TOP_LEFT, 24, 172);

    lv_obj_t *cap_tgt = lv_label_create(root);
    lv_label_set_text(cap_tgt, "TARGET");
    lv_obj_set_style_text_font(cap_tgt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap_tgt, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(cap_tgt, LV_ALIGN_TOP_RIGHT, -24, 152);

    st->target_lbl = lv_label_create(root);
    lv_label_set_text(st->target_lbl, "---");
    lv_obj_set_style_text_font(st->target_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->target_lbl, lv_color_hex(theme.accent), 0);
    lv_obj_align(st->target_lbl, LV_ALIGN_TOP_RIGHT, -24, 172);

    st->backend_lbl = lv_label_create(root);
    lv_label_set_text(st->backend_lbl, "");
    lv_obj_set_style_text_font(st->backend_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->backend_lbl, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(st->backend_lbl, LV_ALIGN_TOP_MID, 0, 240);

    // Adjust row + silence at y=320..380.
    const char *adj_names[4] = {"-10", "-1", "+1", "+10"};
    int adj_vals[4] = {-10, -1, 1, 10};
    int adj_w = 80;
    int adj_y = 320;
    int total_w = adj_w * 4 + 12;  // 4 buttons + 3 gaps of 4
    int adj_x0 = (LCD_W - total_w) / 2;
    for (int i = 0; i < 4; ++i) {
        int x = adj_x0 + i * (adj_w + 4);
        cc_make_button(root, x, adj_y, adj_w, 60, adj_names[i], theme.accent, 0x05101c,
                       cc_adjust_btn_cb, (void *)(intptr_t)adj_vals[i]);
    }
    // Silence button across the full width at the bottom.
    cc_make_button(root, 8, adj_y + 70, LCD_W - 16, 50, "SILENCE", theme.alarm, 0xffffff,
                   cc_silence_btn_cb, nullptr);

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_control_console(lv_obj_t *root, const ScreenVariantSpec &spec,
                                   const sk::Data &data) {
    (void)spec;
    (void)data;
    if (!root) return;
    auto *st = (ControlConsoleState *)lv_obj_get_user_data(root);
    if (!st) return;

    autopilot::State ap;
    autopilot::copy_state(ap);

    // Update mode-button highlights only when the mode actually changes.
    if (ap.mode != st->last_mode) {
        st->last_mode = ap.mode;
        for (int i = 0; i < 4; ++i) {
            bool active = (st->mode_for[i] == ap.mode);
            uint32_t bg = active ? theme.accent : theme.panel;
            uint32_t fg = active ? 0x05101c : theme.fg;
            lv_obj_set_style_bg_color(st->mode_btns[i], lv_color_hex(bg), 0);
            // child label is first child
            lv_obj_t *lbl = lv_obj_get_child(st->mode_btns[i], 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
        }
    }

    char cur[16], tgt[16];
    if (isnan(ap.current_heading_rad))
        snprintf(cur, sizeof(cur), "---");
    else
        snprintf(cur, sizeof(cur), "%03d", (int)(ap.current_heading_rad * 180.0 / M_PI));
    if (isnan(ap.target_heading_rad))
        snprintf(tgt, sizeof(tgt), "---");
    else
        snprintf(tgt, sizeof(tgt), "%03d", (int)(ap.target_heading_rad * 180.0 / M_PI));
    ui::set_text_if_changed(st->current_lbl, st->last_current, sizeof(st->last_current), cur);
    ui::set_text_if_changed(st->target_lbl, st->last_target, sizeof(st->last_target), tgt);
    char bk[32];
    snprintf(bk, sizeof(bk), "backend: %s", autopilot::backend_name(ap.backend));
    lv_label_set_text(st->backend_lbl, bk);
}

// ---------------------------------------------------------------------------
// route_progress template - XTE bar + BTW + DTW summary.
//
// Reads from boat::Snapshot via sk::Data (which composes fused values).
// Layout:
//   top      : "TO WP" caption
//   middle   : BTW (big, accent) | DTW (big)
//   centre   : horizontal XTE bar with port (red) / stbd (green)
//              colored fills, center marker
//   bottom   : XTE numeric (m) + L/R indicator
//
// MetricBinding is unused beyond title - this template doesn't need
// a binding because the SK paths it cares about (XTE, BTW, DTW) are
// fixed. Caller can supply spec.metrics[0] to override the title /
// accent color; otherwise sensible defaults apply.

struct RouteProgressState {
    lv_obj_t *btw_lbl;
    lv_obj_t *dtw_lbl;
    lv_obj_t *xte_bar;
    lv_obj_t *xte_lbl;
    lv_obj_t *centre_marker;
    char last_btw[12];
    char last_dtw[12];
    char last_xte[16];
    double xte_full_scale_m;  // bar saturates at this magnitude
};

static lv_obj_t *create_route_progress(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    RouteProgressState *st =
        (RouteProgressState *)heap_caps_calloc(1, sizeof(RouteProgressState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] route_progress alloc failed");
        return root;
    }
    strncpy(st->last_btw, "\xFF", sizeof(st->last_btw));
    strncpy(st->last_dtw, "\xFF", sizeof(st->last_dtw));
    strncpy(st->last_xte, "\xFF", sizeof(st->last_xte));
    st->xte_full_scale_m = 200.0;  // ±200 m saturates the bar

    // Title at top.
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, spec.title ? spec.title : "ROUTE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // BTW left / DTW right at y=80.
    lv_obj_t *btw_cap = lv_label_create(root);
    lv_label_set_text(btw_cap, "BEARING");
    lv_obj_set_style_text_font(btw_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(btw_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(btw_cap, LV_ALIGN_TOP_LEFT, 24, 76);

    st->btw_lbl = lv_label_create(root);
    lv_label_set_text(st->btw_lbl, "---");
    lv_obj_set_style_text_font(st->btw_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->btw_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->btw_lbl, LV_ALIGN_TOP_LEFT, 24, 96);

    lv_obj_t *dtw_cap = lv_label_create(root);
    lv_label_set_text(dtw_cap, "DISTANCE");
    lv_obj_set_style_text_font(dtw_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dtw_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(dtw_cap, LV_ALIGN_TOP_RIGHT, -24, 76);

    st->dtw_lbl = lv_label_create(root);
    lv_label_set_text(st->dtw_lbl, "---");
    lv_obj_set_style_text_font(st->dtw_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->dtw_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->dtw_lbl, LV_ALIGN_TOP_RIGHT, -24, 96);

    // XTE bar around y=260, full width. Port = red, stbd = green.
    // The bar's value 0..200 represents -fullscale..+fullscale; we
    // recolor based on sign. lv_bar doesn't natively support bipolar
    // so we use the "symmetrical" range trick.
    st->xte_bar = lv_bar_create(root);
    lv_obj_set_size(st->xte_bar, LCD_W - 48, 28);
    lv_obj_align(st->xte_bar, LV_ALIGN_TOP_MID, 0, 260);
    lv_bar_set_mode(st->xte_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(st->xte_bar, -100, 100);
    lv_bar_set_value(st->xte_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(st->xte_bar, lv_color_hex(theme.panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st->xte_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(st->xte_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(st->xte_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(st->xte_bar, lv_color_hex(theme.alarm), LV_PART_INDICATOR);

    // Centre marker line.
    st->centre_marker = lv_obj_create(root);
    lv_obj_set_size(st->centre_marker, 2, 40);
    lv_obj_align(st->centre_marker, LV_ALIGN_TOP_MID, 0, 254);
    lv_obj_set_style_bg_color(st->centre_marker, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(st->centre_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(st->centre_marker, 0, 0);
    lv_obj_clear_flag(st->centre_marker, LV_OBJ_FLAG_SCROLLABLE);

    // XTE numeric below the bar.
    lv_obj_t *xte_cap = lv_label_create(root);
    lv_label_set_text(xte_cap, "XTE");
    lv_obj_set_style_text_font(xte_cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(xte_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(xte_cap, LV_ALIGN_TOP_MID, 0, 304);

    st->xte_lbl = lv_label_create(root);
    lv_label_set_text(st->xte_lbl, "---");
    lv_obj_set_style_text_font(st->xte_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->xte_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->xte_lbl, LV_ALIGN_TOP_MID, 0, 324);

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_route_progress(lv_obj_t *root, const ScreenVariantSpec &spec,
                                  const sk::Data &data) {
    (void)spec;
    if (!root) return;
    auto *st = (RouteProgressState *)lv_obj_get_user_data(root);
    if (!st) return;

    char buf[16];
    // BTW
    if (isnan(data.btw))
        snprintf(buf, sizeof(buf), "---");
    else
        snprintf(buf, sizeof(buf), "%03d", (int)rad_to_deg_pos(data.btw));
    ui::set_text_if_changed(st->btw_lbl, st->last_btw, sizeof(st->last_btw), buf);

    // DTW: show nm if >= 1, otherwise m
    if (isnan(data.dtw))
        snprintf(buf, sizeof(buf), "---");
    else if (data.dtw >= 1852.0)
        snprintf(buf, sizeof(buf), "%.1fnm", data.dtw / 1852.0);
    else
        snprintf(buf, sizeof(buf), "%.0fm", data.dtw);
    ui::set_text_if_changed(st->dtw_lbl, st->last_dtw, sizeof(st->last_dtw), buf);

    // XTE
    if (isnan(data.xte)) {
        snprintf(buf, sizeof(buf), "--");
        lv_bar_set_value(st->xte_bar, 0, LV_ANIM_OFF);
    } else {
        double mag = data.xte / st->xte_full_scale_m * 100.0;
        if (mag > 100) mag = 100;
        if (mag < -100) mag = -100;
        lv_bar_set_value(st->xte_bar, (int32_t)mag, LV_ANIM_OFF);
        // Color: port (red) when negative, stbd (green) when positive.
        uint32_t color = data.xte < 0 ? theme.port : theme.good;
        lv_obj_set_style_bg_color(st->xte_bar, lv_color_hex(color), LV_PART_INDICATOR);
        const char *side = data.xte < 0 ? "L" : "R";
        snprintf(buf, sizeof(buf), "%.0f%s m", fabs(data.xte), data.xte == 0 ? "" : side);
    }
    ui::set_text_if_changed(st->xte_lbl, st->last_xte, sizeof(st->last_xte), buf);
}

// ---------------------------------------------------------------------------
// setup_form template - scrollable list of settings rows.
//
// MVP: hardcoded set of common settings (theme, brightness, audible
// alarms). Each row has a caption + a control widget:
//   theme            -> segmented (day / night / auto)
//   brightness       -> -/+ pair around the current value
//   audible alarms   -> toggle
//   reboot           -> action button
//
// Descriptor-driven schema (SettingKind / SettingSpec per spec 13)
// is a follow-up - this template's contract is "show the settings
// that exist today" so screens can adopt it without their authors
// needing to re-wire every preference.

struct SetupFormState {
    lv_obj_t *theme_btns[3];
    lv_obj_t *brightness_lbl;
    lv_obj_t *audible_btn;
    char last_theme[8];
    uint8_t last_brightness;
    bool last_audible;
};

static void sf_theme_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *t = (const char *)lv_event_get_user_data(e);
    app::Command c;
    c.type = app::CommandType::SetTheme;
    strncpy(c.a, t, sizeof(c.a) - 1);
    app::post(c, 100);
}

static void sf_bright_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int now = (int)ui::brightness() + delta;
    if (now < 20) now = 20;
    if (now > 255) now = 255;
    app::Command c;
    c.type = app::CommandType::SetBrightness;
    c.i = now;
    app::post(c, 100);
}

static void sf_audible_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    beeper::set_audible_alarms(!beeper::audible_alarms_enabled());
}

static void sf_reboot_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app::Command c;
    c.type = app::CommandType::Reboot;
    app::post(c, 100);
}

static lv_obj_t *sf_make_row(lv_obj_t *parent, int y, const char *label, lv_obj_t **out_panel) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LCD_W - 16, 56);
    lv_obj_set_pos(row, 8, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg), 0);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 12, 0);
    if (out_panel) *out_panel = row;
    return row;
}

static lv_obj_t *create_setup_form(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    SetupFormState *st =
        (SetupFormState *)heap_caps_calloc(1, sizeof(SetupFormState), MALLOC_CAP_INTERNAL);
    if (!st) {
        net::logf("[layout] setup_form alloc failed");
        return root;
    }
    strncpy(st->last_theme, "?", sizeof(st->last_theme));
    st->last_brightness = 0;
    st->last_audible = false;

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, spec.title ? spec.title : "SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme.accent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    int y = 60;

    // Theme: 3 segmented buttons inside the row, right-aligned.
    lv_obj_t *theme_row;
    sf_make_row(root, y, "THEME", &theme_row);
    static const char *theme_names[3] = {"DAY", "NIGHT", "AUTO"};
    static const char *theme_vals[3] = {"day", "night", "auto"};
    int seg_w = 70;
    int seg_h = 36;
    for (int i = 0; i < 3; ++i) {
        st->theme_btns[i] = cc_make_button(
            theme_row, LCD_W - 16 - 12 - (3 - i) * (seg_w + 4), (56 - seg_h) / 2, seg_w, seg_h,
            theme_names[i], theme.panel_edge, theme.fg, sf_theme_cb, (void *)theme_vals[i]);
    }
    y += 68;

    // Brightness: -/+ buttons around current value label.
    lv_obj_t *bright_row;
    sf_make_row(root, y, "BRIGHTNESS", &bright_row);
    cc_make_button(bright_row, LCD_W - 16 - 12 - 200, 8, 60, 40, "-", theme.accent, 0x05101c,
                   sf_bright_cb, (void *)(intptr_t)-16);
    st->brightness_lbl = lv_label_create(bright_row);
    lv_label_set_text(st->brightness_lbl, "---");
    lv_obj_set_style_text_font(st->brightness_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(st->brightness_lbl, lv_color_hex(theme.fg), 0);
    lv_obj_align(st->brightness_lbl, LV_ALIGN_RIGHT_MID, -80, 0);
    cc_make_button(bright_row, LCD_W - 16 - 12 - 60, 8, 60, 40, "+", theme.accent, 0x05101c,
                   sf_bright_cb, (void *)(intptr_t)16);
    y += 68;

    // Audible alarms: toggle.
    lv_obj_t *aud_row;
    sf_make_row(root, y, "AUDIBLE ALARMS", &aud_row);
    st->audible_btn = cc_make_button(aud_row, LCD_W - 16 - 12 - 100, 8, 100, 40, "OFF",
                                     theme.panel_edge, theme.fg, sf_audible_cb, nullptr);
    y += 68;

    // Reboot action.
    lv_obj_t *rb_row;
    sf_make_row(root, y, "REBOOT", &rb_row);
    cc_make_button(rb_row, LCD_W - 16 - 12 - 100, 8, 100, 40, "REBOOT", theme.alarm, 0xffffff,
                   sf_reboot_cb, nullptr);

    lv_obj_set_user_data(root, st);
    return root;
}

static void update_setup_form(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    (void)spec;
    (void)data;
    if (!root) return;
    auto *st = (SetupFormState *)lv_obj_get_user_data(root);
    if (!st) return;

    // Read current theme from prefs (cheap; happens at ~5 Hz).
    std::string cur_theme;
    {
        storage::Namespace pu("ui", true);
        cur_theme = pu.get_string("theme", "night");
    }
    if (strcmp(st->last_theme, cur_theme.c_str()) != 0) {
        strncpy(st->last_theme, cur_theme.c_str(), sizeof(st->last_theme));
        static const char *vals[3] = {"day", "night", "auto"};
        for (int i = 0; i < 3; ++i) {
            bool active = strcmp(cur_theme.c_str(), vals[i]) == 0;
            uint32_t bg = active ? theme.accent : theme.panel_edge;
            uint32_t fg = active ? 0x05101c : theme.fg;
            lv_obj_set_style_bg_color(st->theme_btns[i], lv_color_hex(bg), 0);
            lv_obj_t *lbl = lv_obj_get_child(st->theme_btns[i], 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
        }
    }

    uint8_t b = ui::brightness();
    if (b != st->last_brightness) {
        st->last_brightness = b;
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)b);
        lv_label_set_text(st->brightness_lbl, buf);
    }

    bool aud = beeper::audible_alarms_enabled();
    if (aud != st->last_audible) {
        st->last_audible = aud;
        lv_obj_t *lbl = lv_obj_get_child(st->audible_btn, 0);
        if (lbl) lv_label_set_text(lbl, aud ? "ON" : "OFF");
        uint32_t bg = aud ? theme.good : theme.panel_edge;
        lv_obj_set_style_bg_color(st->audible_btn, lv_color_hex(bg), 0);
    }
}

// ---------------------------------------------------------------------------
// Freeform builder — one tile per solver-provided pixel rect.
//
// Reuses build_tile() and the per-tile update logic from the QuadGrid path so
// all painters (numeric, compass, gauge, bar, wind_rose, text, button,
// autopilot) are exercised without duplication. Called directly by
// midl_render; NOT wired into create()/update() dispatch (no TemplateId
// churn needed — and the tile_count is variable, not fixed at 4).
//
// Tile state is heap-allocated (MALLOC_CAP_INTERNAL) and attached via
// lv_obj_set_user_data, matching the QuadGrid pattern.

// Maximum freeform tiles per screen — mirrors the MIDL solver bound so a
// PlacementSet can be mapped 1:1. Spec-derived (decoupled from the legacy
// layout::MAX_TILES_PER_SCREEN, which stays 4 to protect the layout::Config
// size guard); see include/midl_limits.h.
static constexpr int FREEFORM_MAX_TILES = (int)midl::FirmwareLimits::max_tiles_per_screen;

// Per-tile state extends QuadGridTile with the tile's own pixel width so the
// update path can correctly call fit_value_font() without relying on the
// QuadGrid-specific QG_TILE_W constant.
struct FreeformTile {
    QuadGridTile tile;  // reuse the full per-tile widget + cache state
    int tile_w;         // pixel width of this tile (from the solver rect)
};

struct FreeformState {
    int count;
    FreeformTile tiles[FREEFORM_MAX_TILES];
};

// Compile-time footprint guard (Part B): a per-screen FreeformState holds
// tiles[max_tiles_per_screen], so it grows with the spec-derived tile count.
// Keep it under the budget so a bumped maxTiles fails the BUILD, not the device.
static_assert(
    sizeof(FreeformState) <= midl::MIDL_FREEFORM_STATE_BUDGET,
    "FreeformState exceeds MIDL_FREEFORM_STATE_BUDGET; raise the budget or lower maxTiles");

lv_obj_t *create_freeform(lv_obj_t *parent, const ScreenVariantSpec &spec, const Rect *rects) {
    if (!spec.metrics || spec.metric_count < 1 || !rects) return nullptr;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LCD_W, LCD_H);
    if (parent) lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // PSRAM, not internal SRAM: at the spec-derived tile count (9) this struct is
    // ~3 KB, and up to ui::MAX_SCREENS are built eagerly — keeping them in scarce
    // internal SRAM would starve NimBLE/LVGL (the documented starvation trap, in
    // reverse). It is read at the 5 Hz UI refresh, so PSRAM latency is irrelevant.
    FreeformState *st =
        (FreeformState *)heap_caps_calloc(1, sizeof(FreeformState), MALLOC_CAP_SPIRAM);
    if (!st) {
        net::logf("[layout] freeform alloc failed");
        return root;  // empty but valid handle
    }

    int n = spec.metric_count;
    if (n > FREEFORM_MAX_TILES) n = FREEFORM_MAX_TILES;
    st->count = n;

    for (int i = 0; i < n; ++i) {
        const Rect &r = rects[i];
        st->tiles[i].tile = build_tile(root, r.x, r.y, r.w, r.h, spec.metrics[i]);
        st->tiles[i].tile.idx = i;
        st->tiles[i].tile_w = r.w;

        // Tap-to-zoom: mirror the QuadGrid interactive-tile logic.
        const MetricBinding &mb = spec.metrics[i];
        bool interactive =
            mb.zoom_target ? (layout::zoom_action(mb.zoomable, mb.zoom_target) != layout::ZOOM_NONE)
                           : (mb.source != MetricSource::None);
        if (interactive) {
            lv_obj_add_flag(st->tiles[i].tile.root, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(st->tiles[i].tile.root, tile_zoom_action_cb, LV_EVENT_CLICKED,
                                (void *)&spec.metrics[i]);
        }
    }

    lv_obj_set_user_data(root, st);
    return root;
}

void update_freeform(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    if (!root) return;
    auto *st = (FreeformState *)lv_obj_get_user_data(root);
    if (!st) return;

    for (int i = 0; i < st->count; ++i) {
        QuadGridTile &t = st->tiles[i].tile;
        if (t.idx < 0 || t.idx >= spec.metric_count) continue;
        const MetricBinding &m = spec.metrics[t.idx];

        char pri[40], sec[24];
        format_metric(m, data, pri, sizeof(pri), sec, sizeof(sec));

        // Per-kind aux updates — mirrors update_quad_grid exactly.
        switch (t.kind) {
        case WidgetKind::Gauge:
        case WidgetKind::Bar: {
            double scalar = metric_scalar(m, data);
            // Honor an explicit MIDL format.range when present; legacy bindings
            // (range_min==range_max) fall back to the built-in per-source heuristic.
            double frac = binding_unit_fraction(m, scalar);
            int pct = isnan(frac) ? 0 : (int)(frac * 100.0 + 0.5);
            if (t.aux && pct != t.last_aux_pct) {
                if (t.kind == WidgetKind::Gauge)
                    lv_arc_set_value(t.aux, pct);
                else
                    lv_bar_set_value(t.aux, pct, LV_ANIM_OFF);
                t.last_aux_pct = pct;
            }
            // Center label. Mirrors update_quad_grid: a ranged element (explicit
            // MIDL format.range) shows the actual scalar to format.precision so a
            // real gauge — e.g. rudder on [-35,35] — reads its true magnitude;
            // legacy/default-range tiles keep the "%d%%" percent of the heuristic
            // range, matching the editor preview.
            char buf[24];
            if (m.range_min != m.range_max) {
                if (isnan(scalar))
                    snprintf(buf, sizeof(buf), "--");
                else {
                    int dp = m.precision >= 0 ? m.precision : 0;
                    snprintf(buf, sizeof(buf), "%.*f", dp, scalar);
                }
            } else if (isnan(frac)) {
                snprintf(buf, sizeof(buf), "--");
            } else {
                snprintf(buf, sizeof(buf), "%d%%", pct);
            }
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), buf);
            break;
        }
        case WidgetKind::Autopilot:
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.secondary) {
                char tgt[24];
                snprintf(tgt, sizeof(tgt), "target %s", sec[0] ? sec : "---");
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        tgt);
            }
            break;
        case WidgetKind::Button:
            // Static label; no update needed.
            break;
        case WidgetKind::Compass:
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.secondary) {
                char cts[24];
                if (m.extras_count > 0 && m.extras[0].source != MetricSource::None) {
                    MetricBinding eb = {};
                    eb.source = m.extras[0].source;
                    char ep[24], esec[24];
                    format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
                    snprintf(cts, sizeof(cts), "%s %s",
                             m.extras[0].label && m.extras[0].label[0] ? m.extras[0].label : "CTS",
                             ep);
                } else {
                    snprintf(cts, sizeof(cts), "%s", sec);
                }
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        cts);
            }
            {
                MetricBinding hdg = {}, cog = {}, cts = {};
                hdg.source = MetricSource::HDG_deg;
                cog.source = MetricSource::COG_deg;
                cts.source = MetricSource::CTS_deg;
                ui::MarkerSpec live[3] = {
                    {metric_scalar(hdg, data), ui::Glyph::Triangle, true, theme.accent},
                    {metric_scalar(cog, data), ui::Glyph::Triangle, false, theme.good},
                    {metric_scalar(cts, data), ui::Glyph::Diamond, true, theme.alarm},
                };
                ui::marker_ring_update(t.markers, live, 3, /*reference=*/0.0);
            }
            break;
        case WidgetKind::Trend: {
            // Mirrors update_quad_grid: hero number is the live reading; the
            // sparkline gets one normalized 0..100 sample on value-change.
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            if (t.aux) {
                double frac = binding_unit_fraction(m, metric_scalar(m, data));
                if (!isnan(frac)) {
                    int v = (int)(frac * 100.0 + 0.5);
                    if (v != t.last_aux_pct) {
                        lv_chart_series_t *s = lv_chart_get_series_next(t.aux, NULL);
                        if (s) {
                            lv_chart_set_next_value(t.aux, s, v);
                            lv_chart_refresh(t.aux);
                        }
                        t.last_aux_pct = v;
                    }
                }
            }
            break;
        }
        default:
            // Numeric, WindRose, Text — value/secondary text slots.
            if (ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri) &&
                t.kind == WidgetKind::Numeric && m.extras_count == 0) {
                // Use the tile's own width (solver may pick a rect different
                // from the fixed QG_TILE_W). -56 matches the QuadGrid margin.
                fit_value_font(t.value, pri, st->tiles[i].tile_w - 56);
            }
            if (t.value && m.source == MetricSource::VMG_kn) {
                double vmg = mps_to_kn(data.vmg);
                uint32_t col = isnan(vmg) ? theme.fg : (vmg < 0 ? theme.alarm : theme.good);
                lv_obj_set_style_text_color(t.value, lv_color_hex(col), 0);
            }
            if (t.secondary) {
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        sec);
            }
            if (t.kind == WidgetKind::WindRose) {
                MetricBinding awa = {}, twa = {};
                awa.source = MetricSource::AWA_deg;
                twa.source = MetricSource::TWA_deg;
                ui::MarkerSpec wind[2] = {
                    {metric_scalar(awa, data), ui::Glyph::ChevronIn, true, theme.warn},
                    {metric_scalar(twa, data), ui::Glyph::ChevronOut, false, theme.good},
                };
                ui::marker_ring_update(t.markers, wind, 2, /*reference=*/0.0);
            }
            break;
        }

        // Multi-row extras (only meaningful for Numeric kind).
        for (uint8_t e = 0; e < m.extras_count && e < 4; ++e) {
            if (!t.extras[e]) continue;
            MetricBinding eb = {};
            eb.source = m.extras[e].source;
            char ep[24], esec[24];
            format_metric(eb, data, ep, sizeof(ep), esec, sizeof(esec));
            char row[32];
            if (m.extras[e].label && m.extras[e].label[0])
                snprintf(row, sizeof(row), "%s %s", m.extras[e].label, ep);
            else
                snprintf(row, sizeof(row), "%s", ep);
            ui::set_text_if_changed(t.extras[e], t.last_extras[e], sizeof(t.last_extras[e]), row);
        }
    }
}

// ---------------------------------------------------------------------------
// Public factory entry points

lv_obj_t *create(lv_obj_t *parent, const ScreenVariantSpec &spec) {
    switch (spec.template_id) {
    case TemplateId::QuadGrid:
        return create_quad_grid(parent, spec);
    case TemplateId::HeroPlus:
        return create_hero_plus(parent, spec);
    case TemplateId::StatusList:
        return create_status_list(parent, spec);
    case TemplateId::RoundInstrument:
        return create_round_instrument(parent, spec);
    case TemplateId::SplitPair:
        return create_split_pair(parent, spec);
    case TemplateId::TrendChart:
        return create_trend_chart(parent, spec);
    case TemplateId::AlertFocus:
        return create_alert_focus(parent, spec);
    case TemplateId::ControlConsole:
        return create_control_console(parent, spec);
    case TemplateId::RouteProgress:
        return create_route_progress(parent, spec);
    case TemplateId::SetupForm:
        return create_setup_form(parent, spec);
    default:
        net::logf("[layout] template %d not implemented yet", (int)spec.template_id);
        return nullptr;
    }
}

void update(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data) {
    s_fmt = config::format();  // snapshot display formatting for this refresh
    switch (spec.template_id) {
    case TemplateId::QuadGrid:
        update_quad_grid(root, spec, data);
        break;
    case TemplateId::HeroPlus:
        update_hero_plus(root, spec, data);
        break;
    case TemplateId::StatusList:
        update_status_list(root, spec, data);
        break;
    case TemplateId::RoundInstrument:
        update_round_instrument(root, spec, data);
        break;
    case TemplateId::SplitPair:
        update_split_pair(root, spec, data);
        break;
    case TemplateId::TrendChart:
        update_trend_chart(root, spec, data);
        break;
    case TemplateId::AlertFocus:
        update_alert_focus(root, spec, data);
        break;
    case TemplateId::ControlConsole:
        update_control_console(root, spec, data);
        break;
    case TemplateId::RouteProgress:
        update_route_progress(root, spec, data);
        break;
    case TemplateId::SetupForm:
        update_setup_form(root, spec, data);
        break;
    default:
        break;
    }
}

}  // namespace ui::layouts

// ===========================================================================
// Full-screen single-value "zoom" screen. Opened by tapping a dashboard tile
// (ui::layouts::tile_zoom_action_cb sets the file-static g_zoom_target then
// navigates here for an auto-scale field). A dedicated screen (not a layer_top
// overlay) so it composites into
// the active-screen snapshot and is screenshot-verifiable. Tapping anywhere
// returns to the dashboard; swipe-down also works via the global gesture map.
// ===========================================================================
namespace ui::zoom {

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_cap = nullptr;
static lv_obj_t *s_value = nullptr;
static lv_obj_t *s_unit = nullptr;
static char s_last[40] = {(char)0xFF};
static int s_last_kind = -1;  // 0 = big number, 1 = multi-line/long (e.g. position)

static void back_cb(lv_event_t *) {
    ui::show_by_id("dashboard");
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(ui::theme.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, back_cb, LV_EVENT_CLICKED, nullptr);

    s_cap = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_cap, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(s_cap, lv_color_hex(ui::theme.fg_dim), 0);
    lv_obj_align(s_cap, LV_ALIGN_TOP_MID, 0, 60);

    // Big hero value: font_xl_64 scaled ~1.6x (820/256) -> ~100px digits for
    // short numerics. Multi-line / long values (e.g. a two-line lat/lon
    // position) switch to an unscaled medium font in refresh so they fit the
    // view instead of overflowing. Width is bounded to the screen and centered.
    s_value = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_value, &font_xl_64, 0);
    // Pivot the scale about the label's own center; otherwise transform_scale
    // grows from the default (0,0) top-left pivot, pushing the hero digits
    // down-right of the CENTER align and clipping their top edge.
    lv_obj_set_style_transform_pivot_x(s_value, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_value, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(s_value, 410, 0);
    lv_obj_set_style_text_color(s_value, lv_color_hex(ui::theme.fg), 0);
    lv_obj_set_style_text_align(s_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_value, "--");

    s_unit = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_unit, &lv_font_montserrat_38, 0);
    lv_obj_set_style_text_color(s_unit, lv_color_hex(ui::theme.fg_dim), 0);
    lv_obj_align(s_unit, LV_ALIGN_BOTTOM_MID, 0, -70);

    // A subtle hint that tapping returns.
    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "tap to close");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(ui::theme.fg_dim), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    return s_root;
}

void refresh() {
    if (!s_root) return;
    const ui::layouts::MetricBinding &m = ui::layouts::g_zoom_target;
    lv_obj_set_style_text_color(s_value, lv_color_hex(m.accent ? m.accent : ui::theme.fg), 0);
    lv_label_set_text(s_cap, m.label ? m.label : "");
    lv_label_set_text(s_unit, m.unit ? m.unit : "");
    sk::Data d;
    sk::copyData(d);
    char pri[40], sec[64];
    ui::layouts::format_metric(m, d, pri, sizeof(pri), sec, sizeof(sec));

    // Choose the value styling by content: a short scalar gets the big scaled
    // hero font; a multi-line or long value (a two-line position, a time, etc.)
    // gets an unscaled medium font so it fits the screen width.
    bool multiline = strchr(pri, '\n') != nullptr || strlen(pri) > 8;
    int kind = multiline ? 1 : 0;
    if (kind != s_last_kind) {
        s_last_kind = kind;
        if (multiline) {
            lv_obj_set_style_text_font(s_value, &lv_font_montserrat_38, 0);
            lv_obj_set_style_transform_scale(s_value, 256, 0);  // 256 = 1.0x (no scale)
        } else {
            lv_obj_set_style_text_font(s_value, &font_xl_64, 0);
            lv_obj_set_style_transform_scale(s_value, 410, 0);
        }
    }
    ui::set_text_if_changed(s_value, s_last, sizeof(s_last), pri);
}

}  // namespace ui::zoom
