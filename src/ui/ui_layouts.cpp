#include "ui_layouts.h"

#include "layout.h"
#include "midl_limits.h"  // midl::FirmwareLimits (spec-derived tile bound), budget caps
#include "subscription_set.h"
#include "ui_theme.h"
#include "config_runtime.h"
#include "ui_data.h"
#include "metric_value.h"  // ui::layouts::metric_value / metric_unit_fraction (shared resolver)
#include "value_format.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "ui_markers.h"
#include "ui_compass.h"  // ui::numeric_tile for the full-screen wind dial's AWS/AWA/TWS/TWA band
#include "ui_geom.h"     // compass legend overlap test (pure, host-tested)
#include "round_safe.h"  // round-panel content placement (#50, pure, host-tested)
#include "signalk.h"
#include "midl_kinds.h"  // button_route: PUT vs console for a button action (#31)
#include "ui_screens.h"
#include "board_pins.h"
#include "app_events.h"
#include "net.h"
#include "autopilot.h"
#include "beeper.h"

#include "storage.h"
#include "theme_choice.h"
#include "web_pw_new.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Global (main.cpp): true while the current touch contact has dragged past the
// swipe threshold — a drag/swipe, not a tap. Declared at global scope so the
// click handlers below reference ::g_pointer_dragging, not a namespaced symbol.
extern volatile bool g_pointer_dragging;

namespace ui::layouts {

// ---------------------------------------------------------------------------
// Per-tile instance state. The freeform screen root carries a FreeformState
// (one TileState per MIDL element) as user_data, so update_freeform() finds
// its labels/widgets without file-scope statics.

struct TileState {
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
    ui::MarkerRing markers;  // compass steering markers (HDG + CTS, inner ring)
    char last_value[24];
    char last_secondary[24];
    char last_extras[4][32];
    int last_aux_pct;  // last gauge/bar value (0..100); -1 = unset
    int idx;           // metric index into spec.metrics[]
    WidgetKind kind;
    // COG on its OWN outer ring so an HDG≈COG overlap doesn't merge into one
    // faint marker (the inner `markers` ring carries HDG + CTS). Additive
    // trailing field — preserves the gnu++11 aggregate-init order above.
    ui::MarkerRing markers_cog;
    // Hero-scaled fullscreen numeric: when >0, the value label uses font_xl_64
    // scaled via transform_scale to ~hero_h px tall, and the updater re-fits the
    // scale on every value-width change (a 1-digit value scales bigger than a
    // 3-digit one). 0 = normal tile (font-ladder fit, no transform). Additive
    // trailing field; value-inits to 0 for every non-hero tile.
    int hero_h;      // target hero height in px (0 = not a hero tile)
    int hero_max_w;  // width budget for the scaled hero (px)
    // Last applied style.zones colour on the gauge/bar fill + hero text
    // (0 = never styled). Additive trailing field (aggregate-init order).
    uint32_t last_zone_rgb;
    // ---- dial-fidelity wave (all additive trailing; zero-init on legacy tiles).
    // Authored MIDL markers[] ring (rim + vector radii). Empty when the binding
    // carries no markers.
    ui::MarkerRing markers_auth;
    // style.sectors arcs + last-set LVGL angles (dirty cache; INT32_MIN unset).
    lv_obj_t *sector_arcs[MAX_DIAL_SECTORS];
    int sector_last_s[MAX_DIAL_SECTORS];
    int sector_last_e[MAX_DIAL_SECTORS];
    int8_t sector_last_hidden[MAX_DIAL_SECTORS];
    // Round-safe content width (#50): the width the painters laid out in when
    // the content moved into the round-safe rect; 0 = the full tile width.
    int body_w;
    // Round-fit dial (#50), set by build_tile before the painter runs:
    //   > 0  draw the ring at exactly this diameter (no floor size) and keep
    //        ring + markers/zone band inside the fitted circle;
    //   < 0  the fitted circle is too small for a ring: value-only fallback;
    //   0    not on a round panel / not a dial (legacy sizing).
    // The value-only fallback also serves a numeric tile whose round-safe rect
    // is too short for paint_numeric_body; fit_w x fit_h is then the value
    // label's budget, which the update path re-fits its font to.
    int fit_dia;
    int fit_w, fit_h;
    // Text element: the painter's font, the top of fit_text_font's ladder.
    const lv_font_t *text_cap;
};

// Reverse of ui::metric_map::path_to_source(): the canonical SignalK path a
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
    case MetricSource::STW_kn:
        return "navigation.speedThroughWater";
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
    case MetricSource::VMGwind_kn:
        return "performance.velocityMadeGood";
    case MetricSource::Rudder_deg:
        return "steering.rudderAngle";
    case MetricSource::Position:
        return "navigation.position";
    case MetricSource::APState:
        return "steering.autopilot.state";
    case MetricSource::OutsideTemp_C:
        return "environment.outside.temperature";
    case MetricSource::OutsidePressure_hPa:
        return "environment.outside.pressure";
    case MetricSource::Humidity_pct:
        return "environment.outside.relativeHumidity";
    case MetricSource::Roll_deg:
    case MetricSource::Pitch_deg:
        // roll/pitch ride the single attitude object delta.
        return "navigation.attitude";
    case MetricSource::ROT_degmin:
        return "navigation.rateOfTurn";
    case MetricSource::TripLog_nm:
        return "navigation.trip.log";
    case MetricSource::Log_nm:
        return "navigation.log";
    case MetricSource::BattCurrent_A:
        return "electrical.batteries.house.current";
    case MetricSource::BattTemp_C:
        return "electrical.batteries.house.temperature";
    case MetricSource::EngineRpm:
        return "propulsion.main.revolutions";
    case MetricSource::EngineCoolant_C:
        return "propulsion.main.temperature";
    case MetricSource::EngineOilP_bar:
        return "propulsion.main.oilPressure";
    case MetricSource::EngineFuelRate_lph:
        return "propulsion.main.fuel.rate";
    case MetricSource::HDGm_deg:
        return "navigation.headingMagnetic";
    case MetricSource::Variation_deg:
        return "navigation.magneticVariation";
    case MetricSource::EngineHours_h:
        return "propulsion.main.runTime";
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
        // Dynamic-path binding: subscribe the RAW path so the PathStore is fed.
        if (!p && m.path && m.path[0]) out.add(m.path);
        // `bindings.dir` second source (dial pointer).
        const char *dp = source_to_path(m.dir_source);
        if (dp) out.add(dp);
        if (!dp && m.dir_path && m.dir_path[0]) out.add(m.dir_path);
        // NOTE: const/local value bindings subscribe NOTHING by construction —
        // the mapper leaves source == None and path == NULL for them.
        // Authored dial markers: each marker's `dir` binding is a live bearing.
        for (uint8_t k = 0; k < m.marker_count && m.markers; ++k) {
            const char *mp = source_to_path(m.markers[k].dir_source);
            if (mp) out.add(mp);
            if (!mp && m.markers[k].dir_path && m.markers[k].dir_path[0])
                out.add(m.markers[k].dir_path);
        }
        // Bound sector edges (dynamic laylines).
        for (uint8_t k = 0; m.sectors && k < m.sector_count && k < MAX_DIAL_SECTORS; ++k) {
            const DialSector &ds = m.sectors[k];
            const char *fp = source_to_path(ds.from_source);
            if (fp) out.add(fp);
            if (!fp && ds.from_path && ds.from_path[0]) out.add(ds.from_path);
            const char *tp = source_to_path(ds.to_source);
            if (tp) out.add(tp);
            if (!tp && ds.to_path && ds.to_path[0]) out.add(ds.to_path);
        }
        uint8_t ne = m.extras_count;
        if (ne > 4) ne = 4;  // bound to the fixed extras[] array
        for (uint8_t e = 0; e < ne; ++e) {
            const char *pe = source_to_path(m.extras[e].source);
            if (pe) out.add(pe);
        }
        // Composite elements (windrose/wind-steer/autopilot/compass) read extra
        // boat::View fields DIRECTLY — not via a single MetricSource binding — so the
        // per-screen subscription manager would otherwise starve them (wind angle,
        // laylines, helm, course markers stay NaN while only the primary value
        // updates). Request those paths here per kind. Some (current.*, beat/gybe,
        // apTarget) have no MetricSource, so they are literal strings.
        switch (m.kind) {
        case WidgetKind::WindRose:
            out.add("environment.wind.angleApparent");
            out.add("environment.wind.angleTrueWater");
            out.add("environment.wind.speedTrue");
            out.add("navigation.headingTrue");
            out.add("environment.current.setTrue");
            out.add("environment.current.drift");
            out.add("navigation.courseRhumbline.nextPoint.bearingTrue");
            break;
        case WidgetKind::WindSteer:
            out.add("environment.wind.angleApparent");
            out.add("environment.wind.angleTrueWater");
            out.add("environment.wind.speedApparent");
            out.add("environment.wind.speedTrue");
            out.add("performance.beatAngle");
            out.add("performance.gybeAngle");
            out.add("navigation.headingTrue");
            out.add("navigation.courseOverGroundTrue");
            out.add("navigation.courseRhumbline.bearingTrackTrue");
            // Steering tile band + rudder bar + engage chip read these directly.
            out.add("navigation.speedOverGround");
            out.add("navigation.speedThroughWater");
            out.add("navigation.courseRhumbline.velocityMadeGood");
            out.add("performance.velocityMadeGood");
            out.add("steering.rudderAngle");
            out.add("steering.autopilot.state");
            break;
        case WidgetKind::Autopilot:
            out.add("navigation.headingTrue");
            out.add("navigation.courseOverGroundTrue");
            out.add("navigation.speedOverGround");
            out.add("navigation.courseRhumbline.bearingTrackTrue");
            out.add("steering.autopilot.target.headingTrue");
            out.add("steering.autopilot.state");  // mode badge (d.apState)
            out.add("steering.rudderAngle");
            out.add("environment.depth.belowKeel");
            out.add("navigation.speedThroughWater");
            out.add("environment.wind.speedApparent");
            out.add("environment.wind.angleApparent");
            break;
        case WidgetKind::Compass:
            out.add("navigation.courseOverGroundTrue");
            out.add("navigation.courseRhumbline.bearingTrackTrue");
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Metric formatting. Returns the primary value text, and optionally
// fills `secondary` with a short context string. NaN -> "--".

// Display formatting (decimals + k/M scaling per unit class). Refreshed once
// per update() from config; format_metric reads it. All painting runs on the
// single LVGL task, so this file-static needs no lock.
static config::FormatConfig s_fmt;

// ---------------------------------------------------------------------------
// Dynamic-path resolution (audit item 8). A MetricBinding whose enum bridge
// missed carries the RAW SignalK path (m.path); it resolves through this
// injected function (device: PathStore-backed, installed by
// midl_render_apply.cpp) so any numeric path renders instead of "--". Kept as
// an injection point so the host/sim builds of this TU never link the store.
static DynamicResolveFn s_dynamic_resolver = nullptr;
void set_dynamic_resolver(DynamicResolveFn fn) {
    s_dynamic_resolver = fn;
}

// Device-local metric resolver ({kind:"local", id:"..."} bindings). Installed
// by midl_render_apply.cpp (device status APIs); NULL on host/sim builds so
// local bindings degrade to "--" there.
static LocalResolveFn s_local_resolver = nullptr;
void set_local_resolver(LocalResolveFn fn) {
    s_local_resolver = fn;
}
static LocalTextResolveFn s_local_text_resolver = nullptr;
void set_local_text_resolver(LocalTextResolveFn fn) {
    s_local_text_resolver = fn;
}

// Display value of a const/local binding; NaN for PathBind (caller handles) or
// an unknown/unresolvable id. Const values are authored in display units (no
// SI conversion — mirrors the web MockDataProvider const path, data.ts:35).
static double bound_value(const MetricBinding &m, const boat::View & /*d*/) {
    if (m.value_kind == BindKind::ConstBind) return m.const_value;
    if (m.value_kind == BindKind::LocalBind && m.local_id && m.local_id[0] && s_local_resolver)
        return s_local_resolver(m.local_id);
    return NAN;
}

// Display-unit value for a dynamic binding: raw SI from the resolver, then the
// format.unit conversion (convert_si_display). NaN when unresolved/uninstalled.
static double dynamic_display_value(const MetricBinding &m, const boat::View &d) {
    if (!m.path || !m.path[0] || !s_dynamic_resolver) return NAN;
    return convert_si_display(s_dynamic_resolver(m.path, d), m.unit);
}

// Dial-pointer bearing for a `bindings.dir` source, in display degrees.
// Enum sources come through metric_value (already degrees for the *_deg
// family); a dynamic path is assumed to carry a SignalK angle in radians.
static bool binding_has_dir(const MetricBinding &m) {
    return m.dir_source != MetricSource::None || (m.dir_path && m.dir_path[0]);
}
static double binding_dir_deg(const MetricBinding &m, const boat::View &d) {
    if (m.dir_source != MetricSource::None) return metric_value(m.dir_source, d);
    if (m.dir_path && m.dir_path[0] && s_dynamic_resolver) {
        double raw = s_dynamic_resolver(m.dir_path, d);
        if (isnan(raw)) return NAN;
        double deg = raw * (180.0 / M_PI);
        if (deg < 0) deg += 360.0;
        return deg;
    }
    return NAN;
}

// Every MIDL colour token's value in the LIVE theme palette (so day/night flips
// keep working). panel2 is the raised badge fill. The firmware Palette has no
// accent2 slot: it maps to the firmware's own green (`good`), which is close to
// but not always equal to the web accent2.
static ColorTokens live_tokens() {
    ColorTokens t;
    t.panel = theme.panel;
    t.panel2 = theme.badge;
    t.edge = theme.panel_edge;
    t.fg = theme.fg;
    t.dim = theme.fg_dim;
    t.accent = theme.accent;
    t.accent2 = theme.good;
    t.good = theme.good;
    t.warn = theme.warn;
    t.alarm = theme.alarm;
    t.port = theme.port;
    t.starboard = theme.starboard;
    t.tide = theme.tide;
    t.stale = theme.stale;
    t.bg = theme.bg;
    return t;
}

// Resolve an authored colour token against the live theme; Default falls back
// to the caller's colour. Shared by style.zones, markers[] and style.sectors.
static uint32_t token_rgb(ZoneColor c, uint32_t literal, uint32_t fallback) {
    return ui::layouts::token_rgb(c, literal, fallback, live_tokens());
}

// The element's style.color (#45, web resolveOptionalColor), else `fallback`.
// Only an authored colour counts; a legacy baked `accent` does not (see
// authored_rgb). Hero text uses value_color(), which still honours it.
static uint32_t elem_rgb(const MetricBinding &m, uint32_t fallback) {
    return authored_rgb(m, fallback, live_tokens());
}

static uint32_t zone_rgb(const MetricZone &z, uint32_t fallback) {
    return token_rgb(z.color, z.rgb, fallback);
}

static void format_metric(const MetricBinding &m, const boat::View &d, char *primary, size_t pcap,
                          char *secondary, size_t scap) {
    secondary[0] = 0;
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
    case MetricSource::STW_kn:
        vfmt::format_scaled(mps_to_kn(d.stw), s_fmt.speed, primary, pcap);
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
    case MetricSource::VMGwind_kn:
        vfmt::format_scaled(mps_to_kn(d.vmgWind), s_fmt.speed, primary, pcap);
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
    case MetricSource::OutsideTemp_C:
        vfmt::format_scaled(isnan(d.outsideTemp) ? NAN : k_to_c(d.outsideTemp), s_fmt.temperature,
                            primary, pcap);
        break;
    case MetricSource::OutsidePressure_hPa:
        // Barometric convention: whole hectopascals, no decimals ("1013").
        if (!isnan(d.outsidePressure))
            snprintf(primary, pcap, "%.0f", units::pa_to_hpa(d.outsidePressure));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::Humidity_pct:
        if (!isnan(d.humidity))
            snprintf(primary, pcap, "%.0f%%", d.humidity * 100.0);
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::Roll_deg:
        // Heel: magnitude + P/S suffix (+ve roll = starboard heel), mirroring
        // the rudder convention. Upright (<0.5 deg) reads bare.
        if (!isnan(d.roll)) {
            double deg = units::rad_to_deg(d.roll);
            double mag = fabs(deg);
            if (mag < 0.5)
                snprintf(primary, pcap, "0");
            else
                snprintf(primary, pcap, "%.0f%c", mag, deg > 0 ? 'S' : 'P');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::Pitch_deg:
        // Signed degrees, +ve = bow up ('-' glyph exists in every ladder font).
        if (!isnan(d.pitch))
            snprintf(primary, pcap, "%.1f", units::rad_to_deg(d.pitch));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::ROT_degmin:
        if (!isnan(d.rateOfTurn))
            snprintf(primary, pcap, "%.0f", units::radps_to_degmin(d.rateOfTurn));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::TripLog_nm:
        vfmt::format_scaled(isnan(d.tripLog) ? NAN : units::m_to_nm(d.tripLog), s_fmt.distance,
                            primary, pcap);
        break;
    case MetricSource::Log_nm:
        vfmt::format_scaled(isnan(d.totalLog) ? NAN : units::m_to_nm(d.totalLog), s_fmt.distance,
                            primary, pcap);
        break;
    case MetricSource::BattCurrent_A:
        // Signed amps: +ve charge into the bank, -ve discharge.
        if (!isnan(d.battCurrent))
            snprintf(primary, pcap, "%.1f", d.battCurrent);
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::BattTemp_C:
        vfmt::format_scaled(isnan(d.battTemp) ? NAN : k_to_c(d.battTemp), s_fmt.temperature,
                            primary, pcap);
        break;
    case MetricSource::EngineRpm:
        if (!isnan(d.engineRevs))
            snprintf(primary, pcap, "%.0f", units::hz_to_rpm(d.engineRevs));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::EngineCoolant_C:
        vfmt::format_scaled(isnan(d.engineCoolantTemp) ? NAN : k_to_c(d.engineCoolantTemp),
                            s_fmt.temperature, primary, pcap);
        break;
    case MetricSource::EngineOilP_bar:
        // Matches the MIDL library engine screen: format.unit "bar", 1 dp.
        if (!isnan(d.engineOilPressure))
            snprintf(primary, pcap, "%.1f", units::pa_to_bar(d.engineOilPressure));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::EngineFuelRate_lph:
        if (!isnan(d.engineFuelRate))
            snprintf(primary, pcap, "%.1f", units::m3s_to_lph(d.engineFuelRate));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::HDGm_deg:
        if (!isnan(d.headingMag))
            snprintf(primary, pcap, "%03.0f", rad_to_deg_pos(d.headingMag));
        else
            snprintf(primary, pcap, "--");
        break;
    case MetricSource::Variation_deg:
        // Magnitude + E/W suffix (+E convention; true = magnetic + variation).
        if (!isnan(d.variation)) {
            double deg = units::rad_to_deg(d.variation);
            snprintf(primary, pcap, "%.0f%c", fabs(deg), deg >= 0 ? 'E' : 'W');
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    case MetricSource::EngineHours_h:
        if (!isnan(d.engineHours))
            snprintf(primary, pcap, "%.1f", units::s_to_h(d.engineHours));
        else
            snprintf(primary, pcap, "--");
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
        if (m.value_kind == BindKind::ConstBind && m.const_text && m.const_text[0]) {
            // Const STRING literal: rendered verbatim (Text/Button captions).
            snprintf(primary, pcap, "%s", m.const_text);
        } else if (m.value_kind == BindKind::LocalBind && m.local_id && s_local_text_resolver &&
                   s_local_text_resolver(m.local_id, primary, pcap)) {
            // Text-valued local id (net.ip, sk.state, ...): rendered verbatim.
        } else if (m.value_kind != BindKind::PathBind) {
            // Const numeric / local device metric. Display units already; only
            // format.decimals applies (const default: %g renders the literal
            // untouched; local default matches the dynamic-path 1 dp).
            double v = bound_value(m, d);
            if (isnan(v))
                snprintf(primary, pcap, "--");
            else if (m.precision >= 0)
                snprintf(primary, pcap, "%.*f", m.precision, v);
            else if (m.value_kind == BindKind::ConstBind)
                snprintf(primary, pcap, "%g", v);
            else
                snprintf(primary, pcap, "%.1f", v);
        } else if (m.path && m.path[0]) {
            // Dynamic-path binding: any numeric SignalK path served by the
            // PathStore, converted to format.unit (else raw SI). Decimals
            // follow format.decimals/precision; default 1 dp.
            double v = dynamic_display_value(m, d);
            if (isnan(v))
                snprintf(primary, pcap, "--");
            else
                snprintf(primary, pcap, "%.*f", m.precision >= 0 ? m.precision : 1, v);
        } else {
            snprintf(primary, pcap, "--");
        }
        break;
    }

    // format.side (audit item 5): magnitude + P/S suffix per the web renderer
    // (model.ts / dial.ts: positive -> 'S', negative -> 'P'; angles wrapped to
    // [-180,180] first). Applied to dynamic-path bindings (unit-classified) and
    // to the degree enum sources; other enum sources keep their legacy text
    // (XTE/Rudder/AWA already carry their own side conventions).
    if (m.side) {
        bool dynamic = (m.source == MetricSource::None && m.path && m.path[0]);
        bool enum_angle = m.source == MetricSource::AWA_deg || m.source == MetricSource::TWA_deg ||
                          m.source == MetricSource::COG_deg || m.source == MetricSource::HDG_deg ||
                          m.source == MetricSource::BTW_deg || m.source == MetricSource::CTS_deg;
        if (dynamic || enum_angle) {
            double v = enum_angle ? metric_value(m.source, d) : dynamic_display_value(m, d);
            if (!isnan(v)) {
                bool angle = enum_angle || unit_is_angle(m.unit);
                format_side_value(v, angle, m.precision, primary, pcap);
            }
        }
    }
}

// ---- tap-to-zoom -----------------------------------------------------------
// Fullscreen-zoom handler (set by the MIDL render layer): a fullscreen-self
// zoom tap builds a transient single-element full-screen render.
static ZoomFullscreenFn s_zoom_fullscreen_fn = nullptr;
void set_zoom_fullscreen_handler(ZoomFullscreenFn fn) {
    s_zoom_fullscreen_fn = fn;
}

// Route a fullscreen-self (auto-scale) zoom request to the MIDL handler.
static void dispatch_fullscreen_self(const MetricBinding &m) {
    if (m.source == MetricSource::None && !(m.path && m.path[0])) return;
    if (s_zoom_fullscreen_fn) s_zoom_fullscreen_fn(m);
}

// Record-aware navigation (the owning MIDL record is loaded first): post a
// ShowScreen to the UI pump rather than calling ui::show_by_id directly.
static void post_show_screen(const char *target) {
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, target, sizeof(c.a) - 1);
    c.t_post_us = micros();
    app::post(c, 0);
}

// Slice 6: per-field zoom dispatch. Honors the element's `zoomable` /
// `zoom_target`: "auto"/"" (or no target) opens the MIDL fullscreen zoom of
// the field, a "<screenId>" target switches to that screen. Runs on the
// LVGL/UI task (LV_EVENT_CLICKED dispatch).
// g_pointer_dragging (global, set by main.cpp touch read-cb): LVGL fires CLICKED on
// release even for long drags (MIDL tiles are non-scrollable, so scroll_limit can't
// cancel it), so every tap/zoom/button handler ignores the click when it was a drag
// — taps act, swipes scroll/navigate.
static void tile_zoom_action_cb(lv_event_t *e) {
    if (g_pointer_dragging) return;  // drag/swipe, not a tap
    const MetricBinding *m = (const MetricBinding *)lv_event_get_user_data(e);
    if (!m) return;

    // No explicit zoom target (the map_element default for a zoomable
    // element): fullscreen-self zoom through the MIDL handler.
    if (!m->zoom_target) {
        dispatch_fullscreen_self(*m);
        return;
    }

    switch (layout::zoom_action(m->zoomable, m->zoom_target)) {
    case layout::ZOOM_AUTO_SCALE:
        dispatch_fullscreen_self(*m);
        break;
    case layout::ZOOM_SHOW_SCREEN:
        // The pump's ShowScreen loads the owning record and no-ops on a
        // dangling screen ref, so a stale zoom target is harmless.
        post_show_screen(m->zoom_target);
        break;
    case layout::ZOOM_NONE:
    default:
        break;
    }
}

// Numeric value for chart-able metrics, in display units. Returns NaN
// for non-scalar bindings (Position, APState, etc). Thin wrapper over the
// shared resolver (metric_value.h), shared with the MIDL mapper. A dynamic-path
// binding (source None, raw path retained) resolves through the injected
// PathStore resolver + format.unit conversion instead.
static double metric_scalar(const MetricBinding &m, const boat::View &d) {
    if (m.source != MetricSource::None) return metric_value(m.source, d);
    if (m.value_kind != BindKind::PathBind) return bound_value(m, d);
    return dynamic_display_value(m, d);
}

// ---------------------------------------------------------------------------
// Tile painters (one per WidgetKind) + the shared tap handlers.

static void tile_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_pointer_dragging) return;  // drag/swipe, not a tap
    auto *target = (const char *)lv_event_get_user_data(e);
    net::logf("[layout] tile CLICKED target=%s", target ? target : "(null)");
    if (!target || !*target) return;
    post_show_screen(target);
}

// Button element tap handler (MIDL `action`). user_data is the tile's
// MetricBinding* (stable: it points into the screen's arena/spec). A command
// action funnels its target through net::dispatchCommand on the UI task — the
// same path screen_settings uses for `theme`/`demo`, so it is safe here (the
// funnel posts SignalK PUTs onto the net worker itself; it does no blocking
// I/O on this task). A nav action posts ShowScreen, mirroring tile_clicked_cb.
static void button_action_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_pointer_dragging) return;  // drag/swipe, not a tap
    const MetricBinding *m = (const MetricBinding *)lv_event_get_user_data(e);
    if (!m) return;
    if (m->command && m->command[0]) {
        // MIDL dialect split (instruments#31): the command slot carries both
        // `command` and `put` targets. Only a valid SignalK path
        // (midl::kinds::put_target_ok: dot-separated [A-Za-z0-9_-] segments,
        // e.g. the Race screen's "steering.autopilot.tack") is issued as a
        // SignalK PUT through the net-worker queue (the same mechanism
        // autopilot.cpp/knob_menu use). Everything else — console commands,
        // including ones with dotted arguments ("sk 192.168.1.5") and dotted
        // non-paths like "../../x.y" — goes to the dispatch funnel and never
        // reaches the bearer-token PUT URL.
        if (midl::kinds::button_route(m->command) == midl::kinds::ButtonRoute::SignalKPut) {
            // Payload: the authored action.value (JSON-encoded by map_element:
            // number/bool/string) when present, else `true` — the SignalK
            // action-trigger convention (tack/gybe/silence).
            const char *val = (m->action_value && m->action_value[0]) ? m->action_value : "true";
            net::logf("[layout] button PUT path='%s' value=%s", m->command, val);
            app::Command c;
            c.type = app::CommandType::SignalKPut;
            strncpy(c.a, m->command, sizeof(c.a) - 1);
            strncpy(c.b, val, sizeof(c.b) - 1);
            app::post_net(c, 100);
            return;
        }
        if (web_pw_new::is_console_only(m->command)) {
            // Would change the web login with the password on the console only.
            net::logf("[layout] button command='%s' refused (console/Settings only)", m->command);
            return;
        }
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

// Gauge/bar fill fraction (0..1) for one binding. When the binding carries an
// explicit MIDL `format.range` (range_min != range_max), scale v into that
// [min,max] window; otherwise fall back to the built-in per-source heuristic
// (metric_unit_fraction, shared via metric_value.h) so legacy tiles
// (range_min==range_max==0) keep their current behavior byte-for-byte.
static double binding_unit_fraction(const MetricBinding &m, double v) {
    if (m.range_min == m.range_max) return metric_unit_fraction(m.source, v);
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
// overflowing/clipping tiles. Called only on text change.
static void fit_value_font(lv_obj_t *label, const char *txt, int max_w) {
    static const lv_font_t *const ladder[] = {&font_xl_64, &font_b612_48, &font_b612_38,
                                              &font_b612_28};
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

// Build-time hero font for a value drawn INSIDE a dial ring: the largest
// ladder font whose worst-case `sample` (e.g. "888", "88.8") fits `max_w`.
// Sized once against the widest plausible text so the hero doesn't change size
// as the value changes. The old diameter thresholds picked the 64 px font for
// any ring >= 135 px, and on a 480x480 quad tile (ring 140 px) "056" / "12.4"
// ran over the ring and its E/W cardinals.
static const lv_font_t *ring_hero_font(const char *sample, int max_w) {
    static const lv_font_t *const ladder[] = {&font_xl_64, &font_b612_48, &font_b612_38,
                                              &font_b612_28};
    for (const lv_font_t *f : ladder) {
        lv_point_t sz;
        lv_text_get_size(&sz, sample, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) return f;
    }
    return &font_b612_28;
}

// Hero-scale a value label well beyond the 64px ceiling of the enabled fonts,
// using lv_obj_set_style_transform_scale (256 == 1.0x) on font_xl_64. The target
// rendered glyph height is `hero_h` px (the goal: ~60% of a 480px screen ≈ 288px,
// so ~4.5x). The label MUST already use font_xl_64 with a center transform pivot
// (set once in the painter). We measure the 1x string and clamp the scale two ways:
//   * height: scale_h = hero_h * 256 / 64  (the requested hero height)
//   * width:  the scaled text must fit hero_max_w, so scale_w = max_w * 256 / w1x
// and take min(scale_h, scale_w). A 3-digit value ("359") at ~110px wide 1x would
// want scale_h≈1152 (288px tall) but is width-clamped to ~480/110*256≈1116, i.e.
// nearly the full hero height; a 1-digit value stays at the full scale_h. Called
// from the painter (build) and the updater (re-fit on each value-width change).
static void fit_hero_scale(lv_obj_t *label, const char *txt, int hero_h, int hero_max_w) {
    if (!label || hero_h <= 0) return;
    lv_point_t sz;
    lv_text_get_size(&sz, (txt && txt[0]) ? txt : "0", &font_xl_64, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int w1x = sz.x > 0 ? sz.x : 1;
    // font_xl_64 line height is ~64px; use the real metric so the target height is
    // exact regardless of the font's internal ascent/descent.
    int h1x = lv_font_get_line_height(&font_xl_64);
    if (h1x <= 0) h1x = 64;
    long scale_h = (long)hero_h * 256 / h1x;
    long scale_w = hero_max_w > 0 ? (long)hero_max_w * 256 / w1x : scale_h;
    long scale = scale_h < scale_w ? scale_h : scale_w;
    if (scale < 256) scale = 256;    // never shrink below 1x (font already 64px)
    if (scale > 4096) scale = 4096;  // sanity ceiling (~1024px) to bound the canvas
    lv_obj_set_style_transform_scale(label, (int32_t)scale, 0);
}

// Text-tile variant of fit_value_font with a smaller Montserrat ladder. Picks
// the largest font whose measured width (lv_text_get_size over the full,
// possibly multi-line string) fits max_w. Fixes POS clipping on the narrow
// (160 px) freeform/quad text tiles where the lat/lon line was wider than the
// 28 px font. Called only on text change.
static int text_width(const char *txt, const lv_font_t *font) {
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// Ellipsize every line of `txt` at the end so it renders within `max_w` in
// `font` ("0.4.0+196.g8d6..."), writing the result to `out`. UTF-8 safe: a cut
// never splits a multi-byte glyph (the degree sign).
static void ellipsize_lines(const char *txt, const lv_font_t *font, int max_w, char *out,
                            size_t cap) {
    static const char kDots[] = "...";
    size_t o = 0;
    out[0] = 0;
    const char *line = txt;
    while (*line && o + 1 < cap) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[64];
        size_t n = len < sizeof(buf) - sizeof(kDots) ? len : sizeof(buf) - sizeof(kDots);
        memcpy(buf, line, n);
        buf[n] = 0;
        if (n < len || text_width(buf, font) > max_w) {
            // Drop glyphs from the end until "<prefix>..." fits.
            while (n > 0) {
                do {
                    --n;
                } while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
                memcpy(buf + n, kDots, sizeof(kDots));
                if (text_width(buf, font) <= max_w) break;
            }
            if (n == 0) memcpy(buf, kDots, sizeof(kDots));
        }
        size_t bl = strlen(buf);
        if (o + bl + 2 > cap) bl = cap > o + 2 ? cap - o - 2 : 0;
        memcpy(out + o, buf, bl);
        o += bl;
        if (nl && o + 1 < cap) out[o++] = '\n';
        out[o] = 0;
        if (!nl) break;
        line = nl + 1;
    }
}

// Fit a MIDL text element's value to its tile width: the largest device font
// ({48,28,20,14}, capped at the painter's pick `cap`) whose rendered width fits
// `max_w`; if even 14 px is too wide, ellipsize at the end. A text value used
// to keep the smallest font and overflow, and the centred label was clipped
// at both edges (VERSION / BUILD on the status screen). Called on text change.
static void fit_text_font(lv_obj_t *label, const char *txt, int max_w, const lv_font_t *cap) {
    static const lv_font_t *const ladder[] = {&font_b612_48, &font_b612_28, &font_b612_20,
                                              &font_b612_14};
    constexpr int N = (int)(sizeof(ladder) / sizeof(ladder[0]));
    int start = 0;
    while (start < N - 1 && cap && ladder[start] != cap)
        ++start;
    if (cap && ladder[start] != cap) start = 0;  // unknown cap: whole ladder
    for (int i = start; i < N; ++i) {
        if (text_width(txt, ladder[i]) <= max_w) {
            lv_obj_set_style_text_font(label, ladder[i], 0);
            return;
        }
    }
    lv_obj_set_style_text_font(label, ladder[N - 1], 0);
    char cut[96];
    ellipsize_lines(txt, ladder[N - 1], max_w, cut, sizeof(cut));
    lv_label_set_text(label, cut);
}

// Gauge-centre variant of fit_value_font: the ladder is CAPPED at the
// diameter-based size paint_gauge_body picked (48/38/28), so the hero never
// grows past the painter's intent, and steps DOWN when value+unit would spill
// past the dial's inner width (e.g. "1800 rpm" @48 overflowed a 160px dial
// into the neighbouring tile). Always restarts from the cap so the value
// grows back when it shortens. Called only on text change.
static void fit_gauge_center_font(lv_obj_t *label, const char *txt, int dia) {
    static const lv_font_t *const ladder[] = {&font_b612_48, &font_b612_38, &font_b612_28,
                                              &font_b612_20};
    int start = 2;              // mirrors paint_gauge_body's vfont pick
    if (dia >= 110) start = 1;  // montserrat_38
    if (dia >= 160) start = 0;  // montserrat_48
    // Inner width budget: label sits at the dial's vertical centre, where the
    // 10px fill arc bounds it on both sides; keep a couple px of air.
    int max_w = dia - 24;
    if (max_w < 40) max_w = 40;
    for (int i = start; i < 3; ++i) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, ladder[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) {
            lv_obj_set_style_text_font(label, ladder[i], 0);
            return;
        }
    }
    lv_obj_set_style_text_font(label, ladder[3], 0);
}

// Hero-value color: honor the element's authored style.color (MetricBinding.accent,
// 0xRRGGBB) when set, else the theme accent. Restores the per-tile semantic colors
// the glass-cockpit design intends (warn XTE, good depth, etc.); MIDL docs set it
// via style.color → map_element → m.style_color (+ m.accent for a literal). With no
// authored colour, a non-zero legacy accent is still honoured, else the theme accent.
static inline uint32_t value_color(const MetricBinding &m) {
    // style_rgb, not elem_rgb: hero text also honours a legacy baked accent.
    return style_rgb(m, theme.accent, live_tokens());
}

// Label ink on a filled engaged badge of colour `bg`. The theme default
// (`good`) keeps the original near-black ink in every theme; an authored
// style.color gets near-black on a light fill, white on a dark one.
static uint32_t badge_ink(uint32_t bg) {
    if (bg == theme.good) return theme.bg;
    uint32_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 >= 96 ? 0x05101c : 0xffffff;
}

// Blend `fg` over `bg` at pct% (0..100). Used to derive theme-relative tint
// fills (e.g. the autopilot engaged pill) from live palette tokens instead of
// hardcoding a palette-specific color that would survive a theme switch.
static uint32_t mix_rgb(uint32_t fg, uint32_t bg, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int f = (int)((fg >> shift) & 0xFF);
        int b = (int)((bg >> shift) & 0xFF);
        out |= (uint32_t)(b + ((f - b) * pct) / 100) << shift;
    }
    return out;
}

static const lv_font_t *compact_font(const char *sample, int max_w, int max_h);
static void paint_value_only_body(TileState &t, const MetricBinding &m, int w, int h,
                                  const char *sample);

static void paint_numeric_body(TileState &t, const MetricBinding &m, int w, int h) {
    bool has_extras = (m.extras_count > 0);
    if (t.fit_dia < 0) {  // round-safe rect too short for the layout below (#50)
        paint_value_only_body(t, m, w, h, "88.8");
        return;
    }

    // Fullscreen-self zoom: the tile fills the whole 480x480 panel (w>=400 &&
    // h>=400). Render the hero value MUCH larger than the 64px font ceiling by
    // transform-scaling font_xl_64 (set up below; see fit_hero_scale). Target ≈60%
    // of the tile height. Multi-row tiles never go hero (their extras need the
    // vertical room).
    bool fullscreen = (w >= 400 && h >= 400) && !has_extras;

    // Pick value font based on tile height. On 480x480 sunton the quad grid
    // gives ~200 px tiles -> the custom 64px font (hero). Single-field tiles
    // without a secondary/extras line get the big font; tighter tiles step
    // down 48 -> 38 -> 28.
    const lv_font_t *vfont = &font_xl_64;
    if (h < 180 || has_extras) vfont = &font_b612_48;
    if (h < 150) vfont = &font_b612_38;
    if (h < 110) vfont = &font_b612_28;
    // style.size role (audit item 6): S/M/L/XL/Fill map onto the enabled font
    // ladder {14,20,28,48,64}, overriding the height-based auto pick. The
    // fullscreen-zoom hero keeps its transform-scale path regardless.
    if (m.size_role >= 1 && m.size_role <= 5) {
        static const lv_font_t *const kRoleFonts[5] = {&font_b612_14, &font_b612_20, &font_b612_28,
                                                       &font_b612_48, &font_xl_64};
        vfont = kRoleFonts[m.size_role - 1];
    }
    if (fullscreen) vfont = &font_xl_64;  // hero scale needs the scalable digit font
    // Unit font: only 14/20 enabled to keep LVGL glyph cache off
    // internal heap (16/18 sizes blew int_largest from 7668 -> 1908 bytes
    // and starved the network task; see USB-flash diagnostic). On a fullscreen
    // hero the unit rides large-but-secondary (38px).
    const lv_font_t *ufont = (vfont == &font_b612_28) ? &font_b612_14 : &font_b612_20;
    if (fullscreen) ufont = &font_b612_38;

    // Flex row container holds value + unit so they're true bottom-aligned
    // (baselines match for digit fonts since there are no descenders).
    // Previous LV_ALIGN_OUT_RIGHT_BOTTOM put the unit's TOP at the value's
    // BOTTOM corner which made the unit drop below the digits and crowd
    // the next character - that's the overlap users saw.
    // Non-fullscreen tiles use a flex row [value][unit] (shared baseline). The
    // fullscreen-zoom hero instead parents the value DIRECTLY to the tile and
    // centers it, so transform_scale about its own pivot keeps the giant number
    // SCREEN-centered. Putting the hero in a flex row beside the unit shifts the
    // value off the tile centre and the scale amplifies it — that was the
    // off-center/clipped zoomed-number bug.
    lv_obj_t *row = nullptr;
    lv_obj_t *vparent = t.root;
    if (!fullscreen) {
        row = lv_obj_create(t.root);
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
        vparent = row;
    }

    t.value = lv_label_create(vparent);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    if (fullscreen) {
        // Center the hero on the tile so transform_scale (about its own 50/50 pivot)
        // grows it symmetrically around the screen centre.
        lv_obj_align(t.value, LV_ALIGN_CENTER, 0, -8);
        // Tile root: the zoom tile fills the whole panel; make sure its chrome can
        // never scroll the scaled hero into view (cause-1 hardening).
        lv_obj_set_scroll_dir(t.root, LV_DIR_NONE);
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        // Hero label: never scrollable, never a scroll source.
        lv_obj_set_scroll_dir(t.value, LV_DIR_NONE);
        lv_obj_clear_flag(t.value, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(t.value, LV_OBJ_FLAG_SCROLL_CHAIN);
        // Scale the hero number to ~60% of the tile height, width-clamped so a
        // 3-digit value still fits. transform_scale grows about the label's own
        // center (set the pivot first; the default top-left pivot would shove the
        // scaled glyphs down-right and clip them). The actual scale is recomputed
        // per value in update_freeform (fit_hero_scale) since a 1-digit value
        // scales bigger than a 3-digit one. Apply an initial fit here so the first
        // frame is already large.
        t.hero_h = (int)(h * 0.6);
        t.hero_max_w = w - 48;  // leave a small horizontal margin inside the screen
        lv_obj_set_style_transform_pivot_x(t.value, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(t.value, lv_pct(50), 0);
        fit_hero_scale(t.value, "0", t.hero_h, t.hero_max_w);
    }

    if (m.unit && m.unit[0]) {
        // Fullscreen hero: the value is centered on t.root (no flex row), so place
        // the unit on t.root just below the giant number (centered). Non-fullscreen:
        // the unit rides in the flex row, bottom-baseline-aligned next to the value.
        t.unit = lv_label_create(fullscreen ? t.root : row);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, ufont, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
        if (fullscreen) {
            // Below the hero: -8 (hero offset) + half the hero height + a gap.
            lv_obj_align(t.unit, LV_ALIGN_CENTER, 0, -8 + t.hero_h / 2 + 6);
        }
    }

    if (!has_extras) {
        // Secondary tracks the value: it sits directly below, centered.
        // This is the editor's `.num-secondary` placement, NOT the prior
        // bottom-right corner pin. The label follows the number's
        // horizontal center even as the number widens/shrinks.
        t.secondary = lv_label_create(t.root);
        lv_label_set_text(t.secondary, "");
        lv_obj_set_style_text_font(t.secondary, &font_b612_14, 0);
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
            lv_obj_set_style_text_font(t.extras[i], &font_b612_14, 0);
            lv_obj_set_style_text_color(t.extras[i], lv_color_hex(theme.fg), 0);
            lv_obj_align(t.extras[i], LV_ALIGN_TOP_MID, 0, first_y + i * row_h);
            lv_obj_clear_flag(t.extras[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// ---------------------------------------------------------------------------
// Dial-fidelity helpers (MIDL markers[] / style.sectors / style.hull / band).
// Shared by the compass and windrose painters + their update paths.

// Free a heap/PSRAM state struct attached as user_data when its LVGL object is
// deleted. MIDL screens are rebuilt live (apply_all on config push / theme
// switch -> reset_screens -> lv_obj_delete), so states that historically lived
// forever must now ride the object lifetime or they leak on every rebuild.
static void free_user_data_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    void *p = lv_obj_get_user_data(obj);
    if (p) {
        heap_caps_free(p);
        lv_obj_set_user_data(obj, nullptr);
    }
}

// Free a plain heap allocation (e.g. an lv_line points array) on delete.
static void free_alloc_cb(lv_event_t *e) {
    void *p = lv_event_get_user_data(e);
    if (p) heap_caps_free(p);
}

static inline double wrap360(double d) {
    d = fmod(d, 360.0);
    if (d < 0) d += 360.0;
    return d;
}

// Authored marker bearing (display degrees) for this frame: enum bridge ->
// metric_value (already degrees for the *_deg family); dynamic path -> raw
// SignalK radians via the injected resolver; {kind:"const"} -> fixed bearing.
// NaN hides the marker (marker_ring_update contract).
static double marker_dir_deg(const DialMarker &mk, const boat::View &d) {
    if (mk.dir_source != MetricSource::None) return metric_value(mk.dir_source, d);
    if (mk.dir_path && mk.dir_path[0] && s_dynamic_resolver) {
        double raw = s_dynamic_resolver(mk.dir_path, d);
        if (isnan(raw)) return NAN;
        return wrap360(raw * (180.0 / M_PI));
    }
    return mk.dir_const;  // NAN when unset
}

// Fill MarkerSpecs from the binding's authored markers. glyph/colour are fixed
// at build (build_marker_ring contract; theme switches rebuild the screen);
// value_deg is NaN at build and live in update.
static uint8_t authored_marker_specs(const MetricBinding &m, const boat::View *d,
                                     ui::MarkerSpec *specs) {
    uint8_t n = m.marker_count;
    if (!m.markers) return 0;
    if (n > MAX_DIAL_MARKERS) n = MAX_DIAL_MARKERS;
    for (uint8_t i = 0; i < n; ++i) {
        const DialMarker &mk = m.markers[i];
        specs[i].value_deg = d ? marker_dir_deg(mk, *d) : NAN;
        specs[i].glyph = (ui::Glyph)mk.glyph;
        specs[i].filled = true;  // the web glyph set renders filled shapes
        specs[i].color = token_rgb(mk.color, mk.rgb, theme.accent);
    }
    return n;
}

// Build the authored-marker ring: rim markers orbit at `rim_r`, vector markers
// (tide/current style) at `vec_r`. Returns an empty ring when none authored.
static ui::MarkerRing build_authored_ring(lv_obj_t *parent, const MetricBinding &m, int cx, int cy,
                                          int rim_r, int vec_r, bool occlude_lower) {
    ui::MarkerSpec specs[MAX_DIAL_MARKERS];
    uint8_t n = authored_marker_specs(m, nullptr, specs);
    if (!n) return ui::MarkerRing{};
    int radii[MAX_DIAL_MARKERS];
    for (uint8_t i = 0; i < n; ++i)
        radii[i] = m.markers[i].vector ? vec_r : rim_r;
    return ui::build_marker_ring_radii(parent, cx, cy, radii, specs, n, occlude_lower);
}

static void update_authored_ring(ui::MarkerRing &ring, const MetricBinding &m, const boat::View &d,
                                 double reference_deg) {
    if (!ring.count) return;
    ui::MarkerSpec specs[MAX_DIAL_MARKERS];
    uint8_t n = authored_marker_specs(m, &d, specs);
    ui::marker_ring_update(ring, specs, n, reference_deg);
}

// A colored sector arc on a dial (style.sectors — no-go zone / laylines),
// drawn behind the needle/markers. Screen-angle convention: degrees from the
// top, clockwise (matches marker_screen_angle / the web arc() helper).
static lv_obj_t *make_dial_sector(lv_obj_t *parent, int cx, int cy, int radius, int width,
                                  uint32_t color, lv_opa_t opa) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    int d = radius * 2;
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - radius, cy - radius);
    lv_arc_set_rotation(a, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);  // bg track invisible
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    return a;
}

// Place a sector spanning screen angles [t0,t1] (deg from top, +cw); cached so
// the arc is only re-stroked on whole-degree change (LVGL top = 270).
static void set_dial_sector(lv_obj_t *a, double t0, double t1, int *last_s, int *last_e) {
    int s = (int)wrap360(270.0 + t0);
    int e = (int)wrap360(270.0 + t1);
    if (s == *last_s && e == *last_e) return;
    *last_s = s;
    *last_e = e;
    lv_arc_set_bg_angles(a, s, e);
    lv_arc_set_angles(a, s, e);
}

// One sector edge for this frame: bound source (enum -> display degrees, raw
// path -> SignalK radians) or the authored fixed angle. NaN hides the sector.
static double sector_edge_deg(float fixed, MetricSource src, const char *path,
                              const boat::View &d) {
    if (src != MetricSource::None) return metric_value(src, d);
    if (path && path[0]) {
        if (!s_dynamic_resolver) return NAN;
        double raw = s_dynamic_resolver(path, d);
        return isnan(raw) ? NAN : raw * (180.0 / M_PI);
    }
    return fixed;
}

// Build the binding's sector arcs into `arcs`/caches (any dial painter).
// Fixed-edge sectors are stroked immediately; bound-edge sectors start hidden
// and appear on the first refresh once their sources resolve.
static void build_sector_arcs(lv_obj_t *parent, const MetricBinding &m, int cx, int cy, int radius,
                              int width, lv_opa_t opa, lv_obj_t **arcs, int *last_s, int *last_e,
                              int8_t *last_hidden) {
    for (uint8_t i = 0; m.sectors && i < m.sector_count && i < MAX_DIAL_SECTORS; ++i) {
        const DialSector &ds = m.sectors[i];
        lv_obj_t *a = make_dial_sector(parent, cx, cy, radius, width,
                                       token_rgb(ds.color, ds.rgb, theme.accent), opa);
        arcs[i] = a;
        last_s[i] = INT32_MIN;
        last_e[i] = INT32_MIN;
        last_hidden[i] = -1;
        bool fixed = ds.from_source == MetricSource::None && !ds.from_path &&
                     ds.to_source == MetricSource::None && !ds.to_path && !isnan(ds.from) &&
                     !isnan(ds.to);
        if (fixed) {
            set_dial_sector(a, ds.from, ds.to, &last_s[i], &last_e[i]);
            last_hidden[i] = 0;
        } else {
            lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
            last_hidden[i] = 1;
        }
    }
}

// Per-frame sector update: resolves bound edges and rotates by `reference_deg`
// (0 for the north-up round dials; the live heading for the band, per web
// dial.ts:245 heading-relative band sectors).
static void update_sector_arcs(const MetricBinding &m, const boat::View &d, double reference_deg,
                               lv_obj_t *const *arcs, int *last_s, int *last_e,
                               int8_t *last_hidden) {
    for (uint8_t i = 0; m.sectors && i < m.sector_count && i < MAX_DIAL_SECTORS; ++i) {
        if (!arcs[i]) continue;
        const DialSector &ds = m.sectors[i];
        double a = sector_edge_deg(ds.from, ds.from_source, ds.from_path, d);
        double b = sector_edge_deg(ds.to, ds.to_source, ds.to_path, d);
        bool hide = isnan(a) || isnan(b);
        ui::set_hidden_if_changed(arcs[i], &last_hidden[i], hide);
        if (hide) continue;
        set_dial_sector(arcs[i], a - reference_deg, b - reference_deg, &last_s[i], &last_e[i]);
    }
}

// style.hull — boat silhouette (7-point outline, web dial.ts:139-146) centered
// at (cx,cy) in `parent`, scaled by the inner-face radius `s`. The points array
// must outlive the lv_line (LVGL does not copy it): small PSRAM alloc freed on
// the line's LV_EVENT_DELETE.
static void add_hull(lv_obj_t *parent, int cx, int cy, int s) {
    static const float kHull[7][2] = {{-0.18f, 0.40f}, {-0.18f, -0.05f}, {-0.14f, -0.27f},
                                      {0.0f, -0.58f},  {0.14f, -0.27f},  {0.18f, -0.05f},
                                      {0.18f, 0.40f}};
    lv_point_precise_t *pts =
        (lv_point_precise_t *)heap_caps_malloc(7 * sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
    if (!pts) return;  // PSRAM exhausted: skip the decoration, no crash
    for (int i = 0; i < 7; ++i) {
        pts[i].x = cx + (int)lroundf(kHull[i][0] * (float)s);
        pts[i].y = cy + (int)lroundf(kHull[i][1] * (float)s);
    }
    lv_obj_t *ln = lv_line_create(parent);
    lv_line_set_points(ln, pts, 7);
    lv_obj_add_event_cb(ln, free_alloc_cb, LV_EVENT_DELETE, pts);
    lv_obj_set_style_line_width(ln, 3, 0);
    lv_obj_set_style_line_color(ln, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_line_opa(ln, LV_OPA_30, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
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
    // matches the dial's LV_ALIGN_CENTER, 0, +4 (ui::geom, host-tested)
    ui::geom::dial_center(w, h, chrome::panel_border + chrome::panel_pad, cx, cy);
}

// ---------------------------------------------------------------------------
// style.shape "band": the semicircular rolling heading band (web dial.ts:227
// bandSvg / the device autopilot HUD's compass primitive). Reuses
// ui::build_compass (white band + green rail + rotating tick scale + upright
// numerals + red lubber) with the HDG hero below the band centre. Authored
// markers ride a heading-relative ring; authored sectors are heading-relative
// arcs on the band (clipped to the top semicircle by the compass root).
struct CompassBandState {
    ui::Compass cp;
    int scx, scy;         // band centre in tile-root content coordinates
    ui::MarkerRing auth;  // authored markers (reference = live heading)
    lv_obj_t *sector_arcs[MAX_DIAL_SECTORS];
    int sector_last_s[MAX_DIAL_SECTORS];
    int sector_last_e[MAX_DIAL_SECTORS];
    int8_t sector_last_hidden[MAX_DIAL_SECTORS];
    int16_t last_rot;
};

// Build the band into the tile. Returns false when the tile is too small for a
// legible band (caller falls back to the round dial) or PSRAM is exhausted.
static bool paint_compass_band_body(TileState &t, const MetricBinding &m, int w, int h) {
    const int inset = chrome::panel_border + chrome::panel_pad;
    const int cw_avail = w - 2 * inset;
    const int ch_avail = h - 2 * inset;
    const int cap_h = 22;                          // chrome caption row
    const int hero_h = ch_avail >= 220 ? 64 : 40;  // HDG hero under the band
    int region_h = ch_avail - cap_h - hero_h - 6;  // compass region budget
    int cw_fit = 2 * (region_h - 24 + 8);          // width giving cp.h == region_h
    int cw = cw_avail - 4;
    if (cw_fit < cw) cw = cw_fit;
    if (cw < 110 || region_h < 80) return false;  // too small -> round fallback

    // PSRAM state; freed on the compass root's delete (live rebuilds must not
    // leak — CLAUDE.md glyph-canvas/teardown trap, applied to states too).
    CompassBandState *st =
        (CompassBandState *)heap_caps_calloc(1, sizeof(CompassBandState), MALLOC_CAP_SPIRAM);
    if (!st) {
        net::logf("[layout] compass band alloc failed");
        return false;
    }

    int cox = (cw_avail - cw) / 2;
    int coy = cap_h;
    st->cp = ui::build_compass(t.root, cox, coy, cw);
    st->scx = cox + st->cp.cx;
    st->scy = coy + st->cp.cy;
    st->last_rot = INT16_MIN;
    lv_obj_set_user_data(st->cp.root, st);
    lv_obj_add_event_cb(st->cp.root, free_user_data_cb, LV_EVENT_DELETE, nullptr);

    // Authored sectors: children of the compass root (its clip hides the lower
    // half), at the white band centre (r-36; band spans r-10..r-62), slotted at
    // index 2 so they render UNDER the tick scale + numerals. Heading-relative:
    // angles are set per frame in update_compass_band.
    {
        int r_band = st->cp.r - 36;
        int band_w = 36;
        build_sector_arcs(st->cp.root, m, st->cp.cx, st->cp.cy, r_band, band_w, LV_OPA_40,
                          st->sector_arcs, st->sector_last_s, st->sector_last_e,
                          st->sector_last_hidden);
        for (uint8_t i = 0; i < m.sector_count && i < MAX_DIAL_SECTORS; ++i) {
            if (!st->sector_arcs[i]) continue;
            lv_obj_move_to_index(st->sector_arcs[i], 2);
            // All band sectors wait for the first refresh (heading-relative).
            lv_obj_add_flag(st->sector_arcs[i], LV_OBJ_FLAG_HIDDEN);
            st->sector_last_hidden[i] = 1;
            st->sector_last_s[i] = INT32_MIN;
            st->sector_last_e[i] = INT32_MIN;
        }
    }

    // Authored markers: heading-relative ring on the tile root (the compass
    // root would clip glyphs orbiting the rim), lower half occluded like the
    // AP HUD. Band markers all ride the band radius (web bandSvg puts every
    // marker ON the band; no separate vector radius there).
    int r_mark = st->cp.r - ui::kSemiMarkerInset;
    st->auth = build_authored_ring(t.root, m, st->scx, st->scy, r_mark, r_mark,
                                   /*occlude_lower=*/true);

    // HDG hero below the band centre (web bandSvg hero).
    const lv_font_t *hf = &font_xl_64;
    if (hero_h < 64) hf = &font_b612_38;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, hf, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_set_style_text_align(t.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(t.value, cw_avail);
    lv_obj_set_pos(t.value, 0, coy + st->cp.h + 2);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    t.aux = st->cp.root;
    t.last_aux_pct = -2;  // composite sentinel (mirrors winddial/aphud)
    return true;
}

// Per-frame band drive: rotate the tick scale by -heading, relayout the upright
// numerals, refresh hero text, and rotate authored markers/sectors with the
// heading as reference. `pri` is format_metric's primary text for the binding.
static void update_compass_band(TileState &t, const MetricBinding &m, const boat::View &d,
                                const char *pri) {
    CompassBandState *st = t.aux ? (CompassBandState *)lv_obj_get_user_data(t.aux) : nullptr;
    if (!st) return;
    ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
    double hdg = metric_scalar(m, d);
    double ref = isnan(hdg) ? 0.0 : hdg;
    int16_t before = st->last_rot;
    ui::set_rot_if_changed(st->cp.scale, &st->last_rot, ui::deg_to_lvgl(-ref));
    if (st->last_rot != before) ui::compass_layout_labels(st->cp, ref);
    update_authored_ring(st->auth, m, d, ref);
    update_sector_arcs(m, d, ref, st->sector_arcs, st->sector_last_s, st->sector_last_e,
                       st->sector_last_hidden);
}

// Round-fit (#50): the largest font of a compact ladder (down to 14 px, below
// the hero ladders' 28 px floor) whose `sample` fits max_w x max_h.
static const lv_font_t *compact_font(const char *sample, int max_w, int max_h) {
    static const lv_font_t *const ladder[] = {&font_b612_48, &font_b612_38, &font_b612_28,
                                              &font_b612_20, &font_b612_14};
    for (const lv_font_t *f : ladder) {
        lv_point_t sz;
        lv_text_get_size(&sz, sample, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w && sz.y <= max_h) return f;
    }
    return &font_b612_14;
}

// True when the gauge centre text carries its unit glued to the number ("77%",
// "12°") rather than as a separate word.
static inline bool unit_is_glued(const char *u) {
    return u && (u[0] == '%' || u[0] == '\xC2');
}

// Widest text a round-fit gauge's value label can show; sizes its font once at
// build. Mirrors update_gauge_bar with a word unit left out (it gets its own
// label there) and a glued one kept.
static void gauge_value_sample(const MetricBinding &m, char *out, size_t cap) {
    if (m.range_min == m.range_max) {
        snprintf(out, cap, "100%%");
        return;
    }
    const int dp = m.precision >= 0 ? m.precision : 0;
    char lo[24], hi[24];
    snprintf(lo, sizeof(lo), "%.*f", dp, (double)m.range_min);
    snprintf(hi, sizeof(hi), "%.*f", dp, (double)m.range_max);
    const char *num = strlen(lo) > strlen(hi) ? lo : hi;
    snprintf(out, cap, "%s%s", num, m.side ? "P" : unit_is_glued(m.unit) ? m.unit : "");
}

// Round-fit fallback (#50) for the ring painters: the fitted circle cannot hold
// the ring at its minimum size, so the tile shows just the value (and the unit
// when there is room) under the caption, inside the round-safe rect. Only
// t.value / t.unit are created; the update paths skip the missing ring,
// markers and secondary line (all null / empty).
static void paint_value_only_body(TileState &t, const MetricBinding &m, int w, int h,
                                  const char *sample) {
    const int inset = chrome::panel_border + chrome::panel_pad;
    // Caption row at the top of the content box (label at y = 2).
    const int cap_h =
        t.cap ? lv_font_get_line_height(lv_obj_get_style_text_font(t.cap, LV_PART_MAIN)) + 4 : 0;
    const int cw = w - 2 * inset;
    int ch = h - 2 * inset - cap_h;
    // Same height steps as the numeric tiles next to it (paint_numeric_body).
    const int step_h = h < 110 ? 34 : h < 150 ? 46 : 58;
    if (ch > step_h) ch = step_h;
    // A glued unit is part of the value text; a word unit rides beside it.
    const bool word_unit = m.unit && m.unit[0] &&
                           !(t.kind == WidgetKind::Gauge &&
                             (unit_is_glued(m.unit) || m.side || m.range_min == m.range_max));
    int unit_w = 0;
    if (word_unit) {
        lv_point_t usz;
        lv_text_get_size(&usz, m.unit, &font_b612_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        unit_w = usz.x + 4;
    }
    // Prefer a readable value over the unit: drop the unit before going below
    // 20 px.
    const lv_font_t *vfont = compact_font(sample, cw - unit_w, ch);
    bool show_unit = word_unit;
    t.fit_w = cw - unit_w;
    t.fit_h = ch;
    if (word_unit && lv_font_get_line_height(vfont) < lv_font_get_line_height(&font_b612_20)) {
        vfont = compact_font(sample, cw, ch);
        show_unit = false;
        t.fit_w = cw;
    }

    lv_obj_t *row = lv_obj_create(t.root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, cap_h / 2);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    t.value = lv_label_create(row);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    if (show_unit) {
        t.unit = lv_label_create(row);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, &font_b612_14, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Compass widget: round bezel with accent border, heading number in the
// center, small "▲" marker at top, CTS label at bottom. Mirrors editor
// .wpreview .compass.
static void paint_compass_body(TileState &t, const MetricBinding &m, int w, int h) {
    if (t.fit_dia < 0) {
        paint_value_only_body(t, m, w, h, "888");
        return;
    }
    // style.shape "band" -> the semicircular rolling heading band; falls back
    // to this round dial when the tile is too small / PSRAM is exhausted.
    if (m.shape == DialShape::Band && paint_compass_band_body(t, m, w, h)) return;
    // Ring size: reserve top 24 px for chrome caption + marker, bottom
    // 22 px for CTS label. Diameter = min(w,h) - 56 keeps the heading
    // text from colliding with cardinals at 14 px.
    // Round-fit (#50): the ring is sized to the fitted circle, and the face
    // sheds detail as it shrinks. The markers ride 6 px further in so their
    // tips stay inside the circle (round_safe::dial_overhang).
    const bool fit = t.fit_dia > 0;
    const int mk_in = fit ? 6 : 0;
    int dia = fit ? t.fit_dia : ui::geom::dial_ring_dia(w, h, 72);
    // Below these ring sizes the E/S/W cardinals and the inner "HDG" caption
    // no longer clear the heading number.
    const bool fit_north_only = fit && dia < 110;
    const bool fit_no_hcap = fit && dia < 130;
    lv_obj_t *ring = lv_obj_create(t.root);
    lv_obj_set_size(ring, dia, dia);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(ring, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    // style.color replaces the ring and heading pointer (web dial.ts opts.color);
    // markers, sectors and the dir pointer keep their own colours.
    lv_obj_set_style_border_color(ring, lv_color_hex(elem_rgb(m, theme.accent)), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    t.aux = ring;

    // Steering marker set, split across two concentric rings so an HDG≈COG
    // overlap stays legible (they otherwise stack into one faint glyph):
    //   inner ring (dia/2 - 6): HDG (solid accent triangle) + CTS (solid alarm
    //                           diamond).
    //   outer ring (dia/2):     COG alone (hollow good-green triangle), drawn
    //                           AFTER the inner ring so its outline reads on top.
    // Bearings filled live in update from metric_scalar(). Amber is reserved for
    // the AP-target / TWD diamonds so CTS and target stay distinguishable.
    int mcx, mcy;
    dial_center(w, h, mcx, mcy);
    // Diamond slot: the built-in CTS marker (alarm), or — when the element
    // authors a `bindings.dir` — the dir pointer in warn (web dial.ts renders
    // dirDeg as the dashed warn pointer). Glyph colours are fixed at build
    // (build_marker_ring contract); update feeds the matching value.
    uint32_t diamond_color = binding_has_dir(m) ? theme.warn : theme.alarm;
    ui::MarkerSpec inner_markers[2] = {
        {NAN, ui::Glyph::Triangle, true, elem_rgb(m, theme.accent)},  // HDG (style.color)
        {NAN, ui::Glyph::Diamond, true, diamond_color},               // CTS or bindings.dir
    };
    t.markers = ui::build_marker_ring(t.root, mcx, mcy, dia / 2 - 6 - mk_in, inner_markers, 2,
                                      /*occlude_lower=*/false);
    ui::MarkerSpec cog_markers[1] = {
        {NAN, ui::Glyph::Triangle, false, theme.good},  // COG (hollow)
    };
    t.markers_cog = ui::build_marker_ring(t.root, mcx, mcy, dia / 2 - mk_in, cog_markers, 1,
                                          /*occlude_lower=*/false);

    // style.sectors: colored arcs just inside the bezel (web roundHud draws
    // them at fixed screen angles on this north-up face — the needle rotates,
    // not the face). Built before the authored markers/vignette/hero so they
    // stay behind everything (the "behind the needle" contract).
    build_sector_arcs(t.root, m, mcx, mcy, dia / 2 - 5, 6, LV_OPA_70, t.sector_arcs,
                      t.sector_last_s, t.sector_last_e, t.sector_last_hidden);

    // style.hull: boat silhouette on the dial face.
    if (m.hull) add_hull(ring, dia / 2, dia / 2, dia / 2 - 10);

    // Authored MIDL markers[]: rim markers orbit at the bezel, vector markers
    // (tide/current) on an inner ring just outside the centre vignette. Built
    // after the built-in rings so custom glyphs read on top; the hero/vignette
    // created below still paint over the very centre.
    t.markers_auth = build_authored_ring(t.root, m, mcx, mcy, dia / 2 - mk_in, (dia / 2) * 62 / 100,
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
        if (fit_north_only && i != 0) continue;
        lv_obj_t *c = lv_label_create(ring);
        lv_label_set_text(c, cardinals[i]);
        lv_obj_set_style_text_font(c, &font_b612_14, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(i == 0 ? theme.fg : theme.fg_dim), 0);
        lv_obj_align(c, aligns[i], xs[i], ys[i]);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    }

    // Center heading - the dominant element. With the direction marker now
    // outside the ring, the centre number can fill the dial: big tiles get the
    // 64px hero font, stepping down on smaller rings.
    // Fit between the E/W cardinals: each takes ~12 px plus its 2 px inset, and
    // we keep 8 px of air on either side.
    const lv_font_t *vfont = ring_hero_font("888", dia - 2 * 22);
    // Round-fit: with E/W gone the number may use the width between the inner
    // markers' orbit, and it centres on the ring when "HDG" is dropped.
    if (fit_north_only) vfont = compact_font("888", dia - 2 * 12, dia / 2);
    const int vy = fit_no_hcap ? (fit_north_only ? 4 : 0) : 6;

    // Vignette disc behind the centre number: a soft panel-coloured pad so the
    // wide HDG digits (e.g. "359") read on top of, not into, the W/E cardinals
    // that sit at the ring edge. Sized to the central third of the dial; the
    // number paints over it. Radius keeps it clear of the E/W letters' inset.
    {
        int disc = dia * 0.56;
        if (disc < 56) disc = 56;
        if (fit && disc > dia - 8) disc = dia - 8;
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
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    // Nudge the number down a touch so the "HDG" caption rides above it (web ref).
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, vy);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    // Round-fit: the CTS line and the legend sit outside the ring, i.e. outside
    // the fitted circle (under the bezel or on the caption), so they are left
    // out; so is the inner "HDG" caption on a small ring.
    if (fit_no_hcap) return;

    // Small dim "HDG" caption directly above the big heading number, matching the
    // web compass. Positioned at a fixed offset above the ring centre (the value
    // is centre-anchored, so the gap is stable regardless of the live digits'
    // width). The offset scales with the value font's height.
    int vh = lv_font_get_line_height(vfont);
    lv_obj_t *hcap = lv_label_create(ring);
    lv_label_set_text(hcap, "HDG");
    lv_obj_set_style_text_font(hcap, &font_b612_14, 0);
    lv_obj_set_style_text_color(hcap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(hcap, LV_ALIGN_CENTER, 0, 6 - vh / 2 - 8);
    lv_obj_clear_flag(hcap, LV_OBJ_FLAG_CLICKABLE);
    if (fit) return;

    // CTS line tracks the ring's bottom (label-tracks-number geometry).
    t.secondary = lv_label_create(t.root);
    lv_label_set_text(t.secondary, "");
    lv_obj_set_style_text_font(t.secondary, &font_b612_14, 0);
    lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.warn), 0);
    lv_obj_align_to(t.secondary, ring, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);

    // Legend (bottom-left): tells the operator which glyph is which without
    // memorizing color/shape. Three compact rows — cyan▲ HDG, green△ COG, red◆
    // CTS — built from the same ui::draw_glyph the rings use (so the legend glyph
    // matches the rim glyph exactly). A 16px glyph canvas scaled into a small box
    // sits left of each montserrat_14 label in theme.fg_dim. Kept tight in the
    // tile's lower-left corner so it doesn't crowd the dial. Only drawn when the
    // rows really clear the ring, its orbiting marker glyphs and the CTS line
    // (ui::geom::compass_legend_fits). On a 480x480 quad tile they would print
    // across the dial, so it is dropped there; on a tall tile it sits below the
    // ring. The colours/shapes are also explained by the CTS readout.
    const int content_w = w - 2 * (chrome::panel_border + chrome::panel_pad);
    const int content_h = h - 2 * (chrome::panel_border + chrome::panel_pad);
    if (ui::geom::compass_legend_fits(content_w, content_h, mcx, mcy, dia)) {
        struct LegendItem {
            ui::Glyph glyph;
            bool filled;
            uint32_t color;
            const char *txt;
        };
        // Per call, not static: follows style.color (the HDG pointer) and the
        // live theme after a switch.
        const LegendItem kLegend[3] = {
            {ui::Glyph::Triangle, true, elem_rgb(m, theme.accent), "HDG"},
            {ui::Glyph::Triangle, false, theme.good, "COG"},
            {ui::Glyph::Diamond, true, theme.alarm, "CTS"},
        };
        const int row_h = ui::geom::kLegendRowH;
        const int gx = ui::geom::kLegendInset;  // left inset within content box
        const int gy = ui::geom::compass_legend_top(content_h);
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *g =
                ui::draw_glyph(t.root, kLegend[i].glyph, kLegend[i].filled, kLegend[i].color);
            // draw_glyph returns a 28x28 canvas; scale 50% about its center so the
            // visible glyph fills ~14px while the object box stays 28px. The box
            // top-left is pinned at (gx-7, row-7) so the scaled-down content centers
            // at (gx+7, row+7), i.e. a 14px swatch (row..row+14) at the left inset.
            lv_obj_set_style_transform_pivot_x(g, 14, 0);
            lv_obj_set_style_transform_pivot_y(g, 14, 0);
            lv_obj_set_style_transform_scale(g, 128, 0);  // 50% (256 = 100%)
            lv_obj_set_pos(g, gx - 7, gy + i * row_h - 7);
            lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *l = lv_label_create(t.root);
            lv_label_set_text(l, kLegend[i].txt);
            lv_obj_set_style_text_font(l, &font_b612_14, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(theme.fg_dim), 0);
            lv_obj_set_pos(l, gx + 14, gy + i * row_h);
            lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        }
    }
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
static void paint_gauge_body(TileState &t, const MetricBinding &m, int w, int h) {
    const bool ranged = (m.range_min != m.range_max);
    // Round-fit (#50): value-only when the fitted circle cannot hold the ring;
    // else the ring is sized to the circle, the tick labels show only on a
    // ring large enough to keep them clear of the value, and the unit moves
    // from the value text to the arc's open bottom.
    const bool fit = t.fit_dia != 0;
    char sample[32] = "";
    if (fit) gauge_value_sample(m, sample, sizeof(sample));
    if (t.fit_dia < 0) {
        paint_value_only_body(t, m, w, h, sample);
        return;
    }
    int dia = (w < h ? w : h) - 56;
    if (dia < 88) dia = 88;
    if (fit) dia = t.fit_dia;
    const bool tick_labels = ranged && (!fit || dia >= 150);
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
    // Fill follows the hero text: zone colour (update) > style.color > accent.
    lv_obj_set_style_arc_color(arc, lv_color_hex(elem_rgb(m, theme.accent)), LV_PART_INDICATOR);
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
        lv_scale_set_label_show(scale, tick_labels);
        lv_obj_set_style_text_font(scale, &font_b612_14, LV_PART_MAIN);
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

    // style.zones (audit item 4): static threshold bands ringing the gauge just
    // outside the fill arc, each band spanning [prev lt, lt) of the authored
    // range mapped onto the 270° sweep. Painted once at build; the LIVE fill +
    // hero text colour additionally track the active zone in update (web
    // parity: gaugeSvg fill uses zoneColor). Bands need a range to map display
    // units to angles, so they only render on a ranged element.
    if (ranged && m.zone_count > 0 && m.range_max > m.range_min) {
        const float lo = m.range_min, span = m.range_max - m.range_min;
        float prev = lo;
        for (uint8_t zi = 0; zi < m.zone_count && zi < MAX_METRIC_ZONES; ++zi) {
            float zlo = prev < lo ? lo : prev;
            float zhi = m.zones[zi].lt > m.range_max ? m.range_max : m.zones[zi].lt;
            prev = m.zones[zi].lt;
            if (zhi <= zlo) continue;
            int a0 = (135 + (int)lroundf((zlo - lo) / span * 270.0f)) % 360;
            int a1 = (135 + (int)lroundf((zhi - lo) / span * 270.0f)) % 360;
            lv_obj_t *band = lv_arc_create(t.root);
            lv_obj_remove_style(band, NULL, LV_PART_KNOB);
            lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
            int bd = dia + 14;  // just outside the 10px fill arc, inside the tile
            lv_obj_set_size(band, bd, bd);
            lv_obj_align(band, LV_ALIGN_CENTER, 0, 4);
            lv_arc_set_bg_angles(band, a0, a1);
            lv_arc_set_angles(band, a0, a1);
            uint32_t zc = zone_rgb(m.zones[zi], elem_rgb(m, theme.accent));
            lv_obj_set_style_arc_color(band, lv_color_hex(zc), LV_PART_MAIN);
            lv_obj_set_style_arc_color(band, lv_color_hex(zc), LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(band, 4, LV_PART_MAIN);
            lv_obj_set_style_arc_width(band, 4, LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(band, LV_OPA_80, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(band, LV_OPA_80, LV_PART_INDICATOR);
        }
    }

    // Percent label is the gauge's hero number, sized to dominate the
    // arc's open bottom. NO second caption beneath - the chrome already
    // carries the metric label in the tile's top-left (removed the
    // duplicate `cap` label that overlapped the percent on small tiles).
    const lv_font_t *vfont = &font_b612_28;
    if (dia >= 110) vfont = &font_b612_38;
    if (dia >= 160) vfont = &font_b612_48;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 4);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    if (fit) {
        // Value sized once to the ring's inner width (update_gauge_bar does not
        // re-fit a round-fit gauge), nudged up to leave the open bottom to the unit.
        const bool word_unit = ranged && m.unit && m.unit[0] && !unit_is_glued(m.unit) && !m.side;
        const int inner = dia - 2 * 10 - (tick_labels ? 56 : 10);
        const lv_font_t *vf = compact_font(sample, inner, dia / 2);
        lv_obj_set_style_text_font(t.value, vf, 0);
        lv_obj_align(t.value, LV_ALIGN_CENTER, 0, word_unit ? 0 : 4);
        if (word_unit) {
            t.unit = lv_label_create(t.root);
            lv_label_set_text(t.unit, m.unit);
            lv_obj_set_style_text_font(t.unit, &font_b612_14, 0);
            lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
            // In the arc's open bottom; a large ring keeps it under the value.
            if (dia >= 150)
                lv_obj_align(t.unit, LV_ALIGN_CENTER, 0, lv_font_get_line_height(vf) / 2 + 8);
            else
                lv_obj_align(t.unit, LV_ALIGN_CENTER, 0, 4 + dia / 2 - 12);
            lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// True when a gauge's range straddles zero (e.g. rudder [-35,35]) — its fill
// must be centre-anchored at the zero position, not left-anchored.
static inline bool gauge_is_bipolar(const MetricBinding &m) {
    return m.range_min < 0.0 && m.range_max > 0.0;
}

// Center-anchored arc fill for a bipolar gauge. The 270° background sweeps
// 135°(lower-left) → 180 → 270(top) → 0 → 45°(lower-right) clockwise; zero sits
// at the top centre (270°). A positive value fills clockwise from 270° toward
// 45° (right half); a negative value fills counter-clockwise from 270° toward
// 135° (left half). Each half spans 135° of arc, so |v| at the range endpoint
// fills its whole half. Drives the indicator angles directly (NOT
// lv_arc_set_value, which always fills from the background start).
static void gauge_set_bipolar_fill(lv_obj_t *arc, const MetricBinding &m, double v) {
    if (!arc) return;
    const double kZero = 270.0;       // top centre, in LVGL arc degrees
    const double kHalfSweep = 135.0;  // each polarity owns half of the 270° band
    if (isnan(v)) {
        lv_arc_set_angles(arc, (uint16_t)kZero, (uint16_t)kZero);  // empty
        return;
    }
    if (v >= 0.0) {
        double frac = m.range_max > 0.0 ? v / m.range_max : 0.0;
        if (frac > 1.0) frac = 1.0;
        double end = kZero + frac * kHalfSweep;  // 270 → up to 405 (==45)
        lv_arc_set_angles(arc, (uint16_t)lround(kZero), (uint16_t)lround(end));
    } else {
        double frac = m.range_min < 0.0 ? v / m.range_min : 0.0;  // v,min both <0 → positive
        if (frac > 1.0) frac = 1.0;
        double start = kZero - frac * kHalfSweep;  // 270 → down to 135
        lv_arc_set_angles(arc, (uint16_t)lround(start), (uint16_t)lround(kZero));
    }
}

// Bar widget: title row (label left, percent right) + LVGL bar fill.
// Mirrors editor .wpreview .bar.
static void paint_bar_body(TileState &t, const MetricBinding &m, int w, int h) {
    // Bar tile: percent number is the hero element (centered, big), bar
    // sits below it. Percent dominates the tile so the operator can
    // read SOC / fuel / fresh-water at a glance, like editor's preview.
    int bar_h = h < 130 ? 18 : 22;
    int bar_w = w - 32;

    const lv_font_t *vfont = &font_b612_38;
    if (h < 140) vfont = &font_b612_28;
    if (h >= 180) vfont = &font_b612_48;

    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--%");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
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

    // style.center (audit item 7): centre-zero deviation bar (XTE-style needle
    // from the middle — web tiles.ts barSvg center branch; geometry borrowed
    // from ui_compass build_centerzero_strip). The lv_bar stays as the static
    // track (fill parked at 0); a dim centre tick marks zero and the needle
    // (t.aux2) is repositioned in update from dev = (fraction - 0.5) * 2.
    if (m.center_bar) {
        lv_obj_t *mid = lv_obj_create(bar);
        lv_obj_set_size(mid, 2, bar_h - 2);
        lv_obj_set_pos(mid, bar_w / 2 - 1, 0);
        lv_obj_set_style_bg_color(mid, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mid, 0, 0);
        lv_obj_set_style_pad_all(mid, 0, 0);
        lv_obj_set_style_radius(mid, 0, 0);
        lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(mid, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *needle = lv_obj_create(bar);
        lv_obj_set_size(needle, 4, bar_h - 2);
        lv_obj_set_pos(needle, bar_w / 2 - 2, 0);
        lv_obj_set_style_bg_color(needle, lv_color_hex(theme.good), 0);
        lv_obj_set_style_bg_opa(needle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(needle, 0, 0);
        lv_obj_set_style_pad_all(needle, 0, 0);
        lv_obj_set_style_radius(needle, 2, 0);
        lv_obj_clear_flag(needle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(needle, LV_OBJ_FLAG_CLICKABLE);
        t.aux2 = needle;
    }
}

// Gauge/Bar tile refresh. Drives
// the fill (or the centre-zero needle), the centre label, and the style.zones
// threshold colour (web parity: fill + hero text follow the active zone).
static void update_gauge_bar(TileState &t, const MetricBinding &m, const boat::View &data) {
    double scalar = metric_scalar(m, data);
    // Honor an explicit MIDL format.range when present; legacy bindings
    // (range_min==range_max) fall back to the built-in per-source heuristic.
    double frac = binding_unit_fraction(m, scalar);
    int pct = isnan(frac) ? 0 : (int)(frac * 100.0 + 0.5);
    bool bipolar = (t.kind == WidgetKind::Gauge) && gauge_is_bipolar(m);
    bool center_bar = (t.kind == WidgetKind::Bar) && m.center_bar && t.aux2;
    if (center_bar) {
        // Centre-zero deviation needle: dev = (fraction - 0.5) * 2 in [-1, 1];
        // NaN parks at centre (the web renderer's `fraction ?? 0.5`).
        int cpct = isnan(frac) ? 50 : pct;
        if (t.aux && cpct != t.last_aux_pct) {
            int bw = lv_obj_get_width(t.aux) - 2;  // content width inside the 1px border
            if (bw < 8) bw = 8;
            double dev = (cpct / 100.0 - 0.5) * 2.0;
            int x = (int)(bw / 2 + dev * (bw / 2.0)) - 2;
            if (x < 0) x = 0;
            if (x > bw - 4) x = bw - 4;
            lv_obj_set_x(t.aux2, x);
            t.last_aux_pct = cpct;
        }
    } else if (t.aux && pct != t.last_aux_pct) {
        if (t.kind == WidgetKind::Gauge) {
            // Bipolar gauge (range straddles zero, e.g. rudder [-35,35]):
            // fill outward from the top-centre zero. Other gauges keep the
            // standard left-anchored 0..100 fill.
            if (bipolar)
                gauge_set_bipolar_fill(t.aux, m, scalar);
            else
                lv_arc_set_value(t.aux, pct);
        } else {
            lv_bar_set_value(t.aux, pct, LV_ANIM_OFF);
        }
        t.last_aux_pct = pct;
    }
    // Center label. Legacy tiles (no explicit MIDL format.range) show the
    // percent of the heuristic range, matching the editor preview. When the
    // element carries an explicit format.range, show the actual scalar value
    // (formatted to format.precision, or 0 dp by default) so a real gauge —
    // e.g. rudder on [-35,35] — reads its true magnitude, not a percent. With
    // format.side, the magnitude carries the P/S suffix instead of a sign.
    char buf[24];
    if (m.range_min != m.range_max) {
        if (isnan(scalar))
            snprintf(buf, sizeof(buf), "--");
        else if (m.side) {
            format_side_value(scalar, unit_is_angle(m.unit), m.precision, buf, sizeof(buf));
        } else {
            int dp = m.precision >= 0 ? m.precision : 0;
            // Append the unit when authored. No separator before a
            // glued unit ("%", "°"=UTF-8 0xC2 lead byte); a plain ASCII
            // space before word units ("kn"/"V"/"m"). Keeps BATT reading
            // "77%" and rudder "12°", not a bare "77"/"12". Must be a
            // regular space: the bundled Montserrat fonts have no glyph
            // for U+2009 THIN SPACE (or U+2007/U+00A0), which rendered
            // as a tofu box between value and unit.
            // A round-fit gauge (#50) shows a word unit in its own label (or
            // drops it when there is no room), so the value text omits it.
            const bool has_unit =
                m.unit && m.unit[0] && !(t.fit_dia != 0 && !unit_is_glued(m.unit));
            snprintf(buf, sizeof(buf), "%.*f%s%s", dp, scalar,
                     has_unit ? (unit_is_glued(m.unit) ? "" : " ") : "", has_unit ? m.unit : "");
        }
    } else if (isnan(frac)) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", pct);
    }
    if (ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), buf) &&
        t.kind == WidgetKind::Gauge && t.aux && t.fit_dia == 0) {
        // Re-fit the centre label on every text change: a ranged value+unit
        // string ("1800 rpm") can outgrow the dial at the painter's default
        // font size and collide with the neighbouring tile.
        fit_gauge_center_font(t.value, buf, (int)lv_obj_get_style_width(t.aux, LV_PART_MAIN));
    }
    // style.zones: the LIVE fill + hero text colour track the active band
    // (first zone whose lt exceeds the display value; none -> painter default).
    if (m.zone_count > 0) {
        uint32_t fallback = (t.kind == WidgetKind::Bar) ? theme.good : value_color(m);
        int zi = zone_pick(m.zones, m.zone_count, scalar);
        uint32_t zc = (zi >= 0) ? zone_rgb(m.zones[zi], fallback) : fallback;
        if (zc != t.last_zone_rgb) {
            if (t.kind == WidgetKind::Gauge && t.aux) {
                lv_obj_set_style_arc_color(t.aux, lv_color_hex(zc), LV_PART_INDICATOR);
            } else if (center_bar) {
                lv_obj_set_style_bg_color(t.aux2, lv_color_hex(zc), 0);
            } else if (t.aux) {
                lv_obj_set_style_bg_color(t.aux, lv_color_hex(zc), LV_PART_INDICATOR);
            }
            if (t.value) lv_obj_set_style_text_color(t.value, lv_color_hex(zc), 0);
            t.last_zone_rgb = zc;
        }
    }
}

// Trend widget: current reading (hero number) above a time-windowed chart
// (midl docs/specs/2026-11-15-trend-history.md). The chart spans exactly the
// element's history.window with its right edge at "now": one point per time
// bucket (floor(t / step), step = window / points), a gap where a bucket has no
// samples, and it scrolls one bucket per step even when the value is steady.
//
// Samples live in a trend::Store (PSRAM) that holds series ONLY for the screen
// on display: they are created (and backfilled from the SignalK History API)
// when the screen is shown and freed when it is left. Values are stored in SI
// and converted at draw time with the hero number's own conversion. The y
// range is style.range (display units) or auto (visible min/max + 10 %); two
// dim labels at the chart's left edge show its max (top) and min (bottom).

static constexpr int TREND_Y_SCALE = 1000;         // chart y axis 0..1000
static constexpr uint32_t TREND_REDRAW_MS = 1000;  // in-bucket redraw throttle

struct TrendTileState {
    int8_t series;      // trend::Store index; -1 = no history (over maxSeries)
    bool primary;       // the first tile of a shared series records the samples
    uint32_t last_ver;  // series version last drawn
    int64_t last_head;  // series head bucket last drawn
    uint32_t last_draw_ms;
    lv_obj_t *chart;
    lv_obj_t *lbl_hi, *lbl_lo;
    char hi_txt[16], lo_txt[16];
};

static trend::Store s_trend;
static lv_obj_t *s_trend_owner = nullptr;  // freeform root whose series are live
static uint32_t s_trend_gen = 0;           // backfill generation of the owner
static const TrendBackfillOps *s_trend_ops = nullptr;
static float *s_trend_disp = nullptr;  // [points] display-unit scratch (PSRAM)

void set_trend_backfill(const TrendBackfillOps *ops) {
    s_trend_ops = ops;
}

static void *trend_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_DEFAULT);
    return p;
}
static void trend_free(void *p) {
    heap_caps_free(p);
}

static double trend_now_s() {
    if (s_trend_ops && s_trend_ops->now_s) return s_trend_ops->now_s();
    return (double)lv_tick_get() / 1000.0;
}

static bool trend_store_ready() {
    if (!s_trend.init((uint16_t)midl_gen::HISTORY_POINTS, (uint8_t)midl_gen::HISTORY_MAX_SERIES,
                      trend_alloc, trend_free))
        return false;
    if (!s_trend_disp)
        s_trend_disp = (float *)trend_alloc(sizeof(float) * (size_t)midl_gen::HISTORY_POINTS);
    return s_trend_disp != nullptr;
}

void trend_stats(int &series, size_t &bytes) {
    series = s_trend.series_count();
    bytes = (size_t)series * s_trend.points() * (sizeof(float) + sizeof(uint16_t));
}

// SI value of the trend's binding (const/local are display units already).
static double trend_si(const MetricBinding &m, const boat::View &d) {
    if (m.source != MetricSource::None) return metric_si(m.source, d);
    if (m.value_kind != BindKind::PathBind) return bound_value(m, d);
    if (!m.path || !m.path[0] || !s_dynamic_resolver) return NAN;
    return s_dynamic_resolver(m.path, d);
}

// SI -> display units: the same conversion the hero number uses.
static double trend_display(const MetricBinding &m, double si) {
    if (isnan(si)) return NAN;
    if (m.source != MetricSource::None) return metric_display_from_si(m.source, si);
    if (m.value_kind != BindKind::PathBind) return si;
    return convert_si_display(si, m.unit);
}

// Series key (path + agg + window share a series) and the backfill path
// (nullptr for const/local bindings, which are never backfilled).
static const char *trend_key(const MetricBinding &m, char *buf, size_t cap) {
    const char *p = source_to_path(m.source);
    if (p) return p;
    if (m.value_kind == BindKind::LocalBind && m.local_id) {
        snprintf(buf, cap, "local:%s", m.local_id);
        return buf;
    }
    if (m.value_kind == BindKind::ConstBind) {
        snprintf(buf, cap, "const:%g", (double)m.const_value);
        return buf;
    }
    return (m.path && m.path[0]) ? m.path : nullptr;
}

static void trend_reset_chart(TrendTileState *ts) {
    if (!ts || !ts->chart) return;
    lv_chart_series_t *ser = lv_chart_get_series_next(ts->chart, NULL);
    if (ser) lv_chart_set_all_values(ts->chart, ser, LV_CHART_POINT_NONE);
    if (ts->lbl_hi) lv_obj_add_flag(ts->lbl_hi, LV_OBJ_FLAG_HIDDEN);
    if (ts->lbl_lo) lv_obj_add_flag(ts->lbl_lo, LV_OBJ_FLAG_HIDDEN);
    ts->hi_txt[0] = ts->lo_txt[0] = 0;
    ts->last_ver = 0;
    ts->last_head = INT64_MIN;
    ts->last_draw_ms = 0;
}

static void trend_release() {
    s_trend.release_all();
    s_trend_owner = nullptr;
    if (s_trend_gen && s_trend_ops && s_trend_ops->begin) s_trend_ops->begin();  // cancel
    s_trend_gen = 0;
}

void trend_screen_changed() {
    if (s_trend_owner && lv_screen_active() != s_trend_owner) trend_release();
}

static void trend_root_deleted_cb(lv_event_t *e) {
    if ((lv_obj_t *)lv_event_get_target(e) == s_trend_owner) trend_release();
}

static TrendTileState *trend_state(const TileState &t) {
    if (t.kind != WidgetKind::Trend || !t.aux) return nullptr;
    return (TrendTileState *)lv_obj_get_user_data(t.aux);
}

static void paint_trend_body(TileState &t, const MetricBinding &m, int w, int h) {
    // Current reading: hero number, upper third of the tile.
    // Value + format.unit share a bottom-aligned flex row, like a
    // single-value tile ("6.9 kn", not a bare "6.9").
    const lv_font_t *vfont = &font_b612_28;
    if (h >= 160) vfont = &font_b612_38;
    if (w < 140) vfont = &font_b612_20;
    const lv_font_t *ufont = (vfont == &font_b612_38) ? &font_b612_20 : &font_b612_14;
    lv_obj_t *row = lv_obj_create(t.root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    t.value = lv_label_create(row);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    if (m.unit && m.unit[0]) {
        t.unit = lv_label_create(row);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, ufont, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }

    // Chart fills the lower half of the tile: one point per history bucket.
    int chart_w = w - 24;
    int chart_h = h / 2 - 12;
    if (chart_h < 36) chart_h = 36;
    lv_obj_t *chart = lv_chart_create(t.root);
    lv_obj_set_size(chart, chart_w, chart_h);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, (uint32_t)midl_gen::HISTORY_POINTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, TREND_Y_SCALE);
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
    // Before any data: an empty chart (every bucket a gap) and no labels.
    lv_chart_set_all_values(chart, s, LV_CHART_POINT_NONE);
    t.aux = chart;
    t.last_aux_pct = -1;

    // y-range labels: max top-left, min bottom-left, dim, display units.
    auto range_label = [&](lv_align_t al) {
        lv_obj_t *l = lv_label_create(t.root);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, &font_b612_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(theme.fg_dim), 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(l, chart, al, 0, 0);
        return l;
    };
    TrendTileState *ts =
        (TrendTileState *)heap_caps_calloc(1, sizeof(TrendTileState), MALLOC_CAP_SPIRAM);
    if (!ts) return;  // number only; update finds no state
    ts->series = -1;
    ts->chart = chart;
    ts->lbl_hi = range_label(LV_ALIGN_TOP_LEFT);
    ts->lbl_lo = range_label(LV_ALIGN_BOTTOM_LEFT);
    ts->last_head = INT64_MIN;
    lv_obj_set_user_data(chart, ts);
    lv_obj_add_event_cb(chart, free_user_data_cb, LV_EVENT_DELETE, nullptr);
}

// Bind the shown screen's trend tiles to fresh series and queue their
// backfill (one request per series). Drops the previous screen's series.
static void trend_activate(lv_obj_t *root, TileState *const *tiles, int count,
                           const ScreenVariantSpec &spec) {
    trend_release();
    s_trend_owner = root;
    bool any = false;
    for (int i = 0; i < count; ++i)
        if (trend_state(*tiles[i])) any = true;
    if (!any || !trend_store_ready()) return;
    if (s_trend_ops && s_trend_ops->begin) s_trend_gen = s_trend_ops->begin();
    for (int i = 0; i < count; ++i) {
        TrendTileState *ts = trend_state(*tiles[i]);
        if (!ts || tiles[i]->idx < 0 || tiles[i]->idx >= spec.metric_count) continue;
        const MetricBinding &m = spec.metrics[tiles[i]->idx];
        trend_reset_chart(ts);
        char kbuf[trend::KEY_MAX];
        const char *key = trend_key(m, kbuf, sizeof(kbuf));
        uint32_t window =
            trend::effective_window(m.history_window_s, (uint32_t)midl_gen::HISTORY_MAX_WINDOW_S);
        trend::Agg agg =
            (trend::Agg)(m.history_agg <= (uint8_t)trend::Agg::Last ? m.history_agg : 0);
        bool created = false;
        int sidx = key ? s_trend.acquire(key, agg, window, &created) : -1;
        ts->series = (int8_t)sidx;
        ts->primary = created;
        // Over maxSeries (or no binding): the tile shows the number only.
        if (sidx < 0) {
            lv_obj_add_flag(ts->chart, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(ts->chart, LV_OBJ_FLAG_HIDDEN);
        const char *bf_path = source_to_path(m.source);
        if (!bf_path && m.value_kind == BindKind::PathBind && m.path && m.path[0]) bf_path = m.path;
        if (created && bf_path && s_trend_gen && s_trend_ops && s_trend_ops->enqueue)
            s_trend_ops->enqueue(s_trend_gen, (uint8_t)sidx, bf_path, agg, window,
                                 s_trend.series(sidx)->step_s);
    }
    net::logf("[trend] screen %p: %d series x %u points", (void *)root, s_trend.series_count(),
              (unsigned)s_trend.points());
}

// Merge a finished backfill (at most one per refresh) into its series.
static void trend_poll_backfill() {
    if (!s_trend_gen || !s_trend_ops || !s_trend_ops->poll) return;
    trend::backfill::Result r;
    if (!s_trend_ops->poll(s_trend_gen, r)) return;
    int n = s_trend.merge_backfill(r.series, r.t_resp_s, r.age, r.val, r.n);
    if (s_trend_ops->done) s_trend_ops->done();
    net::logf("[trend] backfill merged series %u: %d bucket(s) from %d row(s)", (unsigned)r.series,
              n, r.n);
}

static void trend_draw(TrendTileState *ts, const MetricBinding &m) {
    const int P = s_trend.points();
    int s = ts->series;
    for (int i = 0; i < P; ++i) {
        float v = s_trend.value_at(s, i);
        s_trend_disp[i] = isnan(v) ? NAN : (float)trend_display(m, v);
    }
    double lo = 0, hi = 0;
    bool have = trend::auto_range(s_trend_disp, P, lo, hi);  // false: no data yet
    if (have && m.range_min != m.range_max) {
        lo = m.range_min;
        hi = m.range_max;
    }
    lv_chart_series_t *ser = lv_chart_get_series_next(ts->chart, NULL);
    if (!ser) return;
    int32_t *y = lv_chart_get_y_array(ts->chart, ser);
    if (!y) return;
    double span = hi - lo;
    for (int i = 0; i < P; ++i) {
        float v = s_trend_disp[i];
        if (!have || isnan(v) || !(span > 0)) {
            y[i] = LV_CHART_POINT_NONE;
            continue;
        }
        double f = (v - lo) / span;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        y[i] = (int32_t)lround(f * TREND_Y_SCALE);
    }
    lv_chart_refresh(ts->chart);
    if (!have) {
        lv_obj_add_flag(ts->lbl_hi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ts->lbl_lo, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int dp = m.precision >= 0 ? m.precision : (fabs(span) < 10.0 ? 1 : 0);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.*f", dp, hi);
    ui::set_text_if_changed(ts->lbl_hi, ts->hi_txt, sizeof(ts->hi_txt), buf);
    snprintf(buf, sizeof(buf), "%.*f", dp, lo);
    ui::set_text_if_changed(ts->lbl_lo, ts->lo_txt, sizeof(ts->lo_txt), buf);
    lv_obj_clear_flag(ts->lbl_hi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ts->lbl_lo, LV_OBJ_FLAG_HIDDEN);
}

// Per-refresh trend update: record the sample, scroll on the step, redraw
// when the visible series changed (a scroll redraws at once; a change inside
// the current bucket at most once per TREND_REDRAW_MS).
static void update_trend(TileState &t, const MetricBinding &m, const boat::View &d, double now) {
    TrendTileState *ts = trend_state(t);
    if (!ts || ts->series < 0) return;
    if (ts->primary)
        s_trend.sample(ts->series, now, (float)trend_si(m, d));
    else
        s_trend.advance(ts->series, now);
    const trend::Series *se = s_trend.series(ts->series);
    if (!se || se->version == ts->last_ver) return;
    uint32_t ms = lv_tick_get();
    bool scrolled = se->head != ts->last_head;
    if (!scrolled && ts->last_draw_ms && ms - ts->last_draw_ms < TREND_REDRAW_MS) return;
    trend_draw(ts, m);
    ts->last_ver = se->version;
    ts->last_head = se->head;
    ts->last_draw_ms = ms ? ms : 1;
}

// ---------------------------------------------------------------------------
// Clinometer widget: horizontal-arc bubble level for heel angle. A 90° track
// arc spans the top of the tile (LVGL 225°..315°, upright = 270° top-centre)
// with a ±45° tick scale, a port-red band left of zero and a starboard-green
// band right of zero (theme tokens, so day/night flips keep working). The
// "bubble" indicator slides along the track to the current roll; the hero
// number reads |heel|° with the P/S convention, and pitch rides the secondary
// slot as a small readout. Default-binds to attitude roll when unauthored.
// MIDL token "clinometer" is a firmware extension (not in the catalog yet).

// Bubble orbit inset from the track arc's outer radius (px).
static constexpr int CLINO_BUBBLE_INSET = 12;
static constexpr int CLINO_ARC_Y_OFS = 10;  // arc centre offset below tile centre

// Current roll in display degrees for a clinometer tile: the authored binding
// when present (navigation.attitude bridges to Roll_deg), else the default
// attitude-roll source.
static double clino_roll_deg(const MetricBinding &m, const boat::View &d) {
    if (m.source != MetricSource::None || (m.path && m.path[0])) return metric_scalar(m, d);
    return metric_value(MetricSource::Roll_deg, d);
}

static void paint_clinometer_body(TileState &t, const MetricBinding &m, int w, int h) {
    int dia = (w < h ? w : h) - 48;
    if (dia < 96) dia = 96;

    // Track arc across the top (indicator removed: the bubble is the needle).
    lv_obj_t *arc = lv_arc_create(t.root);
    lv_obj_set_size(arc, dia, dia);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, CLINO_ARC_Y_OFS);
    lv_arc_set_bg_angles(arc, 225, 315);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_style(arc, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(theme.grid), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    t.aux = arc;

    // Port (left, red) / starboard (right, green) bands just outside the
    // track, with a small gap around the zero mark (same idiom as the gauge
    // style.zones bands).
    struct Band {
        int a0, a1;
        uint32_t rgb;
    } bands[2] = {{225, 268, theme.port}, {272, 315, theme.starboard}};
    for (const Band &b : bands) {
        lv_obj_t *band = lv_arc_create(t.root);
        lv_obj_remove_style(band, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
        int bd = dia + 12;
        lv_obj_set_size(band, bd, bd);
        lv_obj_align(band, LV_ALIGN_CENTER, 0, CLINO_ARC_Y_OFS);
        lv_arc_set_bg_angles(band, b.a0, b.a1);
        lv_arc_set_angles(band, b.a0, b.a1);
        lv_obj_set_style_arc_color(band, lv_color_hex(b.rgb), LV_PART_MAIN);
        lv_obj_set_style_arc_color(band, lv_color_hex(b.rgb), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(band, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(band, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(band, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(band, LV_OPA_80, LV_PART_INDICATOR);
    }

    // ±45° tick scale ringing the track (labels at -45 / 0 / +45).
    lv_obj_t *scale = lv_scale_create(t.root);
    lv_obj_set_size(scale, dia, dia);
    lv_obj_align(scale, LV_ALIGN_CENTER, 0, CLINO_ARC_Y_OFS);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_angle_range(scale, 90);
    lv_scale_set_rotation(scale, 225);
    lv_scale_set_range(scale, -45, 45);
    lv_scale_set_total_tick_count(scale, 7);  // every 15°
    lv_scale_set_major_tick_every(scale, 3);
    lv_scale_set_label_show(scale, true);
    lv_obj_set_style_text_font(scale, &font_b612_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scale, lv_color_hex(theme.fg_dim), LV_PART_MAIN);
    lv_obj_set_style_length(scale, 6, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(theme.fg_dim), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scale, LV_OBJ_FLAG_SCROLLABLE);

    // Bubble indicator (parked at top-centre until data arrives).
    lv_obj_t *bubble = lv_obj_create(t.root);
    lv_obj_set_size(bubble, 16, 16);
    lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(theme.accent), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bubble, LV_ALIGN_CENTER, 0, CLINO_ARC_Y_OFS - (dia / 2 - CLINO_BUBBLE_INSET));
    t.aux2 = bubble;

    // Hero heel readout at the arc centre; pitch as the small secondary line.
    const lv_font_t *vfont = (dia >= 150) ? &font_b612_48 : &font_b612_38;
    if (h < 140) vfont = &font_b612_28;
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, CLINO_ARC_Y_OFS + 6);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    t.secondary = lv_label_create(t.root);
    lv_label_set_text(t.secondary, "");
    lv_obj_set_style_text_font(t.secondary, &font_b612_14, 0);
    lv_obj_set_style_text_color(t.secondary, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(t.secondary, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_clear_flag(t.secondary, LV_OBJ_FLAG_CLICKABLE);

    t.last_aux_pct = -1;  // deci-degree cache: first update always positions
}

// Shared quad-grid/freeform refresh for a Clinometer tile: bubble position +
// hero heel text + pitch secondary. Bubble/text colour follows the heel side
// (port red / starboard green from the theme), neutral accent when upright.
static void update_clinometer(TileState &t, const MetricBinding &m, const boat::View &data) {
    double roll = clino_roll_deg(m, data);
    // Heel text: magnitude + P/S (see format_metric Roll_deg convention).
    char buf[16];
    if (isnan(roll)) {
        snprintf(buf, sizeof(buf), "--");
    } else if (fabs(roll) < 0.5) {
        snprintf(buf, sizeof(buf), "0\xC2\xB0");
    } else {
        snprintf(buf, sizeof(buf), "%.0f\xC2\xB0%c", fabs(roll), roll > 0 ? 'S' : 'P');
    }
    ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), buf);

    // Bubble: slide along the track to 270° + clamp(roll, ±45°). Dirty-guard
    // on deci-degrees via last_aux_pct (offset by 900 to stay non-negative;
    // -1 = never positioned).
    if (t.aux && t.aux2) {
        double c = isnan(roll) ? 0.0 : roll;
        if (c < -45.0) c = -45.0;
        if (c > 45.0) c = 45.0;
        int deci = (int)lround(c * 10.0) + 900;
        if (deci != t.last_aux_pct) {
            int dia = lv_obj_get_width(t.aux);
            int rb = dia / 2 - CLINO_BUBBLE_INSET;
            double a = (270.0 + c) * M_PI / 180.0;  // LVGL degrees: cw, 0=3 o'clock
            int dx = (int)lround(rb * cos(a));
            int dy = (int)lround(rb * sin(a));
            lv_obj_align(t.aux2, LV_ALIGN_CENTER, dx, CLINO_ARC_Y_OFS + dy);
            t.last_aux_pct = deci;
        }
        uint32_t col = theme.accent;
        if (!isnan(roll) && roll <= -0.5) col = theme.port;
        if (!isnan(roll) && roll >= 0.5) col = theme.starboard;
        if (col != t.last_zone_rgb) {
            lv_obj_set_style_bg_color(t.aux2, lv_color_hex(col), 0);
            if (t.value) lv_obj_set_style_text_color(t.value, lv_color_hex(col), 0);
            t.last_zone_rgb = col;
        }
    }

    // Pitch secondary (always the typed attitude pitch; +ve = bow up).
    if (t.secondary) {
        double pitch = metric_value(MetricSource::Pitch_deg, data);
        char sec[24];
        if (isnan(pitch))
            snprintf(sec, sizeof(sec), "PITCH --");
        else
            snprintf(sec, sizeof(sec), "PITCH %.0f\xC2\xB0", pitch);
        ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary), sec);
    }
}

// ---------------------------------------------------------------------------
// Full-screen wind dial (windrose element on a single-leaf full-screen screen).
//
// Ported from the retired C++ screen_wind.cpp. The legacy screen uses ~15 file-static
// lv_obj_t* handles; here all dial state lives in a PSRAM-allocated
// WindDialState attached to the dial root's user_data, so a windrose tile can
// be instantiated more than once (multiple screens) without file-scope state.
// The dial reads boat::View directly (it is NOT bound through MetricBinding),
// exactly like the legacy screen. Geometry is derived from the passed (w, h)
// of the tile rect (≈ the full 480x480 screen), with the dial centre at the
// tile's local centre — NOT LCD_W/LCD_H — so positions stay tile-relative.

struct WindDialState {
    // Geometry (local to the dial root)
    int cx, cy;
    int r_bezel, r_face, r_closehauled, r_marker;

    // Object handles
    lv_obj_t *root;
    lv_obj_t *bezel;
    lv_obj_t *awa_marker;
    lv_obj_t *twa_marker;
    lv_obj_t *tide_arrow;
    lv_obj_t *tide_zero;
    lv_obj_t *waypoint_marker;
    lv_obj_t *card_lbl[8];
    // Bottom band tiles (AWA / TWS / TWA — AWS is now the centre hero, web ref).
    // tile_aws stays declared for the (now-removed) AWS band tile but is null on
    // the dial; the live AWS readout is lbl_aws_value at the dial centre.
    lv_obj_t *tile_aws, *tile_awa, *tile_tws, *tile_twa;
    lv_obj_t *lbl_aws_value;  // centre hero (apparent wind speed, kn, amber)
    lv_obj_t *lbl_hdg_value, *lbl_tide_speed, *lbl_drift_cap;
    bool show_band;  // false on a short (wind_steer) rect: drop the bottom band

    // Dirty caches (mirror screen_wind.cpp's file-statics)
    char last_aws[16];
    char last_tws[16];
    char last_awa[16];
    char last_twa[16];
    char last_hdg[16];
    char last_tide[16];
    int16_t last_awa_rot;
    int16_t last_twa_rot;
    int16_t last_bezel_rot;
    int16_t last_tide_rot;
    int16_t last_wp_rot;
    int8_t last_awa_hidden;
    int8_t last_twa_hidden;
    int8_t last_tide_hidden;
    int8_t last_tide_zero_hidden;
    int8_t last_wp_hidden;
    int8_t last_drift_cap_hidden;
    int8_t last_tide_speed_hidden;  // speed label has a HUD backing: hide it with no current
};

namespace winddial {

static const int kCardBearing[8] = {0, 45, 90, 135, 180, 225, 270, 315};
static const char *kCardText[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

// Position `o` so its tail sits `dist` above (cx, cy) in the PARENT'S coords,
// with the rotation pivot at the tail. Rotating `o` then sweeps it around
// (cx, cy).
static void apply_pivot_at(lv_obj_t *o, int cx, int cy, int half_w, int dist) {
    lv_obj_set_pos(o, cx - half_w, cy - dist);
    lv_obj_set_style_transform_pivot_x(o, half_w, 0);
    lv_obj_set_style_transform_pivot_y(o, dist, 0);
}

static lv_obj_t *make_ring_at(lv_obj_t *p, int cx, int cy, int diameter, int border, uint32_t color,
                              int opa) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_set_pos(r, cx - diameter / 2, cy - diameter / 2);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(r, opa, 0);
    lv_obj_set_style_border_width(r, border, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static lv_obj_t *make_tick_at(lv_obj_t *parent, int cx, int cy, int angle_deg, int len, int width,
                              uint32_t color, int r_bezel) {
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_size(t, width, len);
    lv_obj_set_style_bg_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 1, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_at(t, cx, cy, width / 2, r_bezel - 4);
    lv_obj_set_style_transform_rotation(t, angle_deg * 10, 0);
    return t;
}

static void build_cardinals(WindDialState *st, lv_obj_t *parent) {
    for (int i = 0; i < 8; ++i) {
        bool card = (i % 2) == 0;  // N / E / S / W
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, kCardText[i]);
        lv_obj_set_style_text_font(l, card ? &font_b612_20 : &font_b612_14, 0);
        uint32_t color = card ? theme.band_ink : theme.band_ink_dim;
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_set_width(l, 40);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        st->card_lbl[i] = l;
    }
}

// Reposition the upright cardinals on the white band for the current heading
// (screen angle = bearing - heading, 0 = up).
static void layout_cardinals(WindDialState *st, double hdg_deg) {
    int R = st->r_bezel - 13;
    for (int i = 0; i < 8; ++i) {
        if (!st->card_lbl[i]) continue;
        double a = (kCardBearing[i] - hdg_deg) * M_PI / 180.0;
        int x = st->cx + (int)(R * sin(a));
        int y = st->cy - (int)(R * cos(a));
        int hh = ((i % 2) == 0) ? 13 : 10;
        lv_obj_set_pos(st->card_lbl[i], x - 20, y - hh);
        lv_obj_clear_flag(st->card_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_bezel(WindDialState *st, lv_obj_t *parent) {
    int R_BEZEL = st->r_bezel;
    st->bezel = lv_obj_create(parent);
    int size = R_BEZEL * 2 + 12;
    int bcx = size / 2;
    int bcy = size / 2;
    lv_obj_set_size(st->bezel, size, size);
    lv_obj_set_pos(st->bezel, st->cx - bcx, st->cy - bcy);
    lv_obj_set_style_bg_opa(st->bezel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st->bezel, 0, 0);
    lv_obj_set_style_pad_all(st->bezel, 0, 0);
    lv_obj_clear_flag(st->bezel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->bezel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(st->bezel, bcx, 0);
    lv_obj_set_style_transform_pivot_y(st->bezel, bcy, 0);

    make_ring_at(st->bezel, bcx, bcy, R_BEZEL * 2, 26, theme.arc_band, LV_OPA_90);
    make_ring_at(st->bezel, bcx, bcy, R_BEZEL * 2 + 18, 7, theme.good, LV_OPA_80);
    make_ring_at(st->bezel, bcx, bcy, R_BEZEL * 2 - 26, 1, 0x0c1828, LV_OPA_COVER);

    for (int deg = 0; deg < 360; deg += 45) {
        make_tick_at(st->bezel, bcx, bcy, deg + 22, 10, 2, theme.band_ink_dim, R_BEZEL);
    }

    // Fixed bow indicator (red triangle) outside the bezel group so it stays at top.
    lv_obj_t *bow_notch = lv_label_create(parent);
    lv_label_set_text(bow_notch, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(bow_notch, &font_b612_20, 0);
    lv_obj_set_style_text_color(bow_notch, lv_color_hex(theme.alarm), 0);
    lv_obj_set_pos(bow_notch, st->cx - 9, st->cy - R_BEZEL - 6);
}

static void build_close_hauled(WindDialState *st, lv_obj_t *parent) {
    int R = st->r_closehauled;
    int d = R * 2;
    lv_obj_t *port = lv_arc_create(parent);
    lv_obj_remove_style(port, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(port, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(port, d, d);
    lv_obj_set_pos(port, st->cx - R, st->cy - R);
    lv_arc_set_rotation(port, 0);
    lv_arc_set_bg_angles(port, 240, 270);  // -30° to bow
    lv_arc_set_angles(port, 240, 270);
    lv_obj_set_style_arc_color(port, lv_color_hex(theme.port), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(port, lv_color_hex(theme.port), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(port, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(port, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_width(port, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(port, 6, LV_PART_MAIN);

    lv_obj_t *stbd = lv_arc_create(parent);
    lv_obj_remove_style(stbd, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(stbd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(stbd, d, d);
    lv_obj_set_pos(stbd, st->cx - R, st->cy - R);
    lv_arc_set_rotation(stbd, 0);
    lv_arc_set_bg_angles(stbd, 270, 300);  // bow to +30°
    lv_arc_set_angles(stbd, 270, 300);
    lv_obj_set_style_arc_color(stbd, lv_color_hex(theme.starboard), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(stbd, lv_color_hex(theme.starboard), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(stbd, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(stbd, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_width(stbd, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(stbd, 6, LV_PART_MAIN);
}

static void build_boat(WindDialState *st, lv_obj_t *parent) {
    int CX = st->cx, CY = st->cy, R_FACE = st->r_face;
    lv_obj_t *cl = lv_obj_create(parent);
    lv_obj_set_size(cl, 1, R_FACE);
    lv_obj_set_pos(cl, CX, CY - R_FACE / 2);
    lv_obj_set_style_bg_color(cl, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_bg_opa(cl, LV_OPA_20, 0);
    lv_obj_set_style_border_width(cl, 0, 0);
    lv_obj_set_style_pad_all(cl, 0, 0);
    lv_obj_clear_flag(cl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cl, LV_OBJ_FLAG_CLICKABLE);

    int hw = R_FACE * 20 / 130;
    int bow = R_FACE * 56 / 130;
    int mid = R_FACE * 26 / 130;
    int shoulder = R_FACE * 4 / 130;
    int stern = R_FACE * 40 / 130;
    // Hull point ring is owned by the line object (LVGL copies points on set in
    // v9, but to be safe store in the state-adjacent heap). A static would be a
    // multi-instance hazard, so allocate alongside the dial in PSRAM.
    lv_point_precise_t *pts =
        (lv_point_precise_t *)heap_caps_calloc(7, sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
    if (!pts) return;
    pts[0] = {(lv_value_precise_t)(CX - hw), (lv_value_precise_t)(CY + stern)};
    pts[1] = {(lv_value_precise_t)(CX - hw), (lv_value_precise_t)(CY - shoulder)};
    pts[2] = {(lv_value_precise_t)(CX - hw * 22 / 28), (lv_value_precise_t)(CY - mid)};
    pts[3] = {(lv_value_precise_t)CX, (lv_value_precise_t)(CY - bow)};
    pts[4] = {(lv_value_precise_t)(CX + hw * 22 / 28), (lv_value_precise_t)(CY - mid)};
    pts[5] = {(lv_value_precise_t)(CX + hw), (lv_value_precise_t)(CY - shoulder)};
    pts[6] = {(lv_value_precise_t)(CX + hw), (lv_value_precise_t)(CY + stern)};
    lv_obj_t *hull = lv_line_create(parent);
    lv_line_set_points(hull, pts, 7);
    lv_obj_set_style_line_color(hull, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_line_opa(hull, LV_OPA_30, 0);
    lv_obj_set_style_line_width(hull, 3, 0);
    lv_obj_set_style_line_rounded(hull, true, 0);
    lv_obj_clear_flag(hull, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *make_wind_marker(WindDialState *st, lv_obj_t *parent, const char *letter,
                                  uint32_t color) {
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_set_size(m, 34, 76);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_at(m, st->cx, st->cy, 17, st->r_marker);

    lv_obj_t *tri = lv_label_create(m);
    lv_label_set_text(tri, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(tri, &font_b612_28, 0);
    lv_obj_set_style_text_color(tri, lv_color_hex(color), 0);
    lv_obj_align(tri, LV_ALIGN_TOP_MID, 0, -6);

    lv_obj_t *stem = lv_obj_create(m);
    lv_obj_set_size(stem, 6, 34);
    lv_obj_set_style_bg_color(stem, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stem, 0, 0);
    lv_obj_set_style_radius(stem, 2, 0);
    lv_obj_set_style_pad_all(stem, 0, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *l = lv_label_create(m);
    lv_label_set_text(l, letter);
    lv_obj_set_style_text_font(l, &font_b612_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 54);
    return m;
}

static void build_tide(WindDialState *st, lv_obj_t *parent) {
    const int TIDE_H = 132;
    const int TIDE_MID = TIDE_H / 2;
    st->tide_arrow = lv_obj_create(parent);
    lv_obj_set_size(st->tide_arrow, 32, TIDE_H);
    lv_obj_set_style_bg_opa(st->tide_arrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st->tide_arrow, 0, 0);
    lv_obj_set_style_pad_all(st->tide_arrow, 0, 0);
    lv_obj_clear_flag(st->tide_arrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->tide_arrow, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_at(st->tide_arrow, st->cx, st->cy, 16, TIDE_MID);

    lv_obj_t *shaft = lv_obj_create(st->tide_arrow);
    lv_obj_set_size(shaft, 5, 54);
    lv_obj_set_pos(shaft, 16 - 2, TIDE_MID - 54);
    lv_obj_set_style_bg_color(shaft, lv_color_hex(theme.tide), 0);
    lv_obj_set_style_bg_opa(shaft, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shaft, 0, 0);
    lv_obj_set_style_radius(shaft, 2, 0);
    lv_obj_set_style_pad_all(shaft, 0, 0);
    lv_obj_clear_flag(shaft, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(shaft, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_label_create(st->tide_arrow);
    lv_label_set_text(head, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(head, &font_b612_28, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(theme.tide), 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, -4);
    lv_obj_add_flag(st->tide_arrow, LV_OBJ_FLAG_HIDDEN);

    st->tide_zero = lv_obj_create(parent);
    lv_obj_set_size(st->tide_zero, 26, 26);
    apply_pivot_at(st->tide_zero, st->cx, st->cy, 13, 13);
    lv_obj_set_style_bg_opa(st->tide_zero, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(st->tide_zero, lv_color_hex(theme.tide), 0);
    lv_obj_set_style_border_width(st->tide_zero, 3, 0);
    lv_obj_set_style_radius(st->tide_zero, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(st->tide_zero, 0, 0);
    lv_obj_clear_flag(st->tide_zero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->tide_zero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st->tide_zero, LV_OBJ_FLAG_HIDDEN);
}

static void build_waypoint(WindDialState *st, lv_obj_t *parent) {
    st->waypoint_marker = lv_obj_create(parent);
    lv_obj_set_size(st->waypoint_marker, 14, 18);
    lv_obj_set_style_bg_color(st->waypoint_marker, lv_color_hex(0xffd21f), 0);
    lv_obj_set_style_bg_opa(st->waypoint_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(st->waypoint_marker, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(st->waypoint_marker, 1, 0);
    lv_obj_set_style_radius(st->waypoint_marker, 4, 0);
    lv_obj_set_style_pad_all(st->waypoint_marker, 0, 0);
    lv_obj_clear_flag(st->waypoint_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->waypoint_marker, LV_OBJ_FLAG_CLICKABLE);
    apply_pivot_at(st->waypoint_marker, st->cx, st->cy, 7, st->r_bezel + 4);
    lv_obj_add_flag(st->waypoint_marker, LV_OBJ_FLAG_HIDDEN);
}

// Bottom tile band (AWA / TWS / TWA), pinned to the bottom of the dial root and
// spanning its width — matches the web design (AWS is the centre hero, so it is
// NOT duplicated in the band). Three tiles evenly spaced across the width.
// `band_h` is the full band height (TILE_BAND).
static void build_tiles(WindDialState *st, lv_obj_t *parent, int w, int h, int band_h) {
    const lv_font_t *tv = &font_b612_38;
    int gap = 8, n = 3;
    int tw = (w - gap * (n + 1)) / n;
    int th = band_h - 16;
    if (th < 40) th = 40;
    int ty = h - th - 8;
    st->tile_aws = nullptr;  // AWS now lives at the dial centre, not in the band
    st->tile_awa = ui::numeric_tile(parent, gap, ty, tw, th, "AWA", "", tv, theme.fg);
    st->tile_tws = ui::numeric_tile(parent, gap * 2 + tw, ty, tw, th, "TWS", "kn", tv, theme.fg);
    st->tile_twa = ui::numeric_tile(parent, gap * 3 + tw * 2, ty, tw, th, "TWA", "", tv, theme.fg);
}

static int16_t deg_to_lvgl(double deg) {
    int16_t r = (int16_t)(lround(deg) * 10);
    while (r < 0)
        r += 3600;
    while (r >= 3600)
        r -= 3600;
    return r;
}

// Build the dial into `parent` (the tile root) filling its w x h content area.
// Returns a PSRAM WindDialState* (also attached to the dial root's user_data).
// `hero_rgb`: the AWS hero colour (element style.color, else theme.warn).
static WindDialState *build(lv_obj_t *parent, int w, int h, uint32_t hero_rgb) {
    WindDialState *st =
        (WindDialState *)heap_caps_calloc(1, sizeof(WindDialState), MALLOC_CAP_SPIRAM);
    if (!st) {
        net::logf("[layout] wind dial alloc failed");
        return nullptr;
    }
    // Sentinel-init the dirty caches (calloc gives 0; force "unset").
    st->last_aws[0] = (char)0xFF;
    st->last_tws[0] = (char)0xFF;
    st->last_awa[0] = (char)0xFF;
    st->last_twa[0] = (char)0xFF;
    st->last_hdg[0] = (char)0xFF;
    st->last_tide[0] = (char)0xFF;
    st->last_awa_rot = INT16_MIN;
    st->last_twa_rot = INT16_MIN;
    st->last_bezel_rot = INT16_MIN;
    st->last_tide_rot = INT16_MIN;
    st->last_wp_rot = INT16_MIN;
    st->last_awa_hidden = -1;
    st->last_twa_hidden = -1;
    st->last_tide_hidden = -1;
    st->last_tide_zero_hidden = -1;
    st->last_wp_hidden = -1;
    st->last_drift_cap_hidden = -1;
    st->last_tide_speed_hidden = -1;

    // Geometry: reserve a bottom tile band; centre the rose in the area above it.
    // Derived from the tile's local (w, h), NOT LCD_W/LCD_H, so the dial is
    // tile-relative and the offset is implicit (children are positioned in the
    // parent's content coordinates).
    const int TILE_BAND = 132;
    // On a short rect (wind_steer: dial shares the screen with a button row, so
    // the dial cell is well under full height) the bottom AWA/TWS/TWA band would
    // squeeze the rose into stacked rows. Mirror the AP-HUD fix: drop the band so
    // the dial gets the full height — its own centre AWS + markers carry the wind
    // info, and the band is redundant on a cramped steering layout.
    st->show_band = (h >= 440);
    int rose_h = st->show_band ? h - TILE_BAND : h;
    if (rose_h < 120) rose_h = h;  // degenerate guard: skip band on tiny rects
    st->cx = w / 2;
    st->cy = rose_h / 2;
    int shortside = (w < rose_h ? w : rose_h);
    st->r_bezel = shortside / 2 - 16;
    st->r_face = st->r_bezel - 28;
    st->r_closehauled = st->r_bezel - 43;
    st->r_marker = st->r_bezel - 18;

    int CX = st->cx, CY = st->cy, R_FACE = st->r_face;

    // Dial root: a transparent, borderless, no-pad container filling the tile so
    // it can hold the rose + bottom tiles, and so update_* can find it by
    // user_data. (The tile chrome is already suppressed by the caller.)
    st->root = lv_obj_create(parent);
    lv_obj_set_size(st->root, w, h);
    lv_obj_set_pos(st->root, 0, 0);
    lv_obj_set_style_bg_opa(st->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st->root, 0, 0);
    lv_obj_set_style_radius(st->root, 0, 0);
    lv_obj_set_style_pad_all(st->root, 0, 0);
    lv_obj_clear_flag(st->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st->root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *r = st->root;

    // Layer order: face -> close-hauled -> boat -> tide -> markers -> bezel
    // -> waypoint -> centre labels -> tiles.
    make_ring_at(r, CX, CY, R_FACE * 2, 0, theme.panel, LV_OPA_COVER);  // face bg ring
    lv_obj_t *face = lv_obj_create(r);
    lv_obj_set_size(face, R_FACE * 2, R_FACE * 2);
    lv_obj_set_pos(face, CX - R_FACE, CY - R_FACE);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_opa(face, LV_OPA_60, 0);
    lv_obj_set_style_border_width(face, 1, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);

    build_close_hauled(st, r);
    build_boat(st, r);
    build_tide(st, r);

    st->lbl_drift_cap = lv_label_create(r);
    lv_label_set_text(st->lbl_drift_cap, "DRIFT");
    lv_obj_set_style_text_font(st->lbl_drift_cap, &font_b612_14, 0);
    lv_obj_set_style_text_color(st->lbl_drift_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(st->lbl_drift_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_drift_cap, 60);
    ui::style_hud_backing(st->lbl_drift_cap);
    lv_obj_set_pos(st->lbl_drift_cap, CX - 30, CY + 60);
    lv_obj_add_flag(st->lbl_drift_cap, LV_OBJ_FLAG_HIDDEN);

    st->lbl_tide_speed = lv_label_create(r);
    lv_label_set_text(st->lbl_tide_speed, "");
    lv_obj_set_style_text_font(st->lbl_tide_speed, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_tide_speed, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(st->lbl_tide_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_tide_speed, 60);
    ui::style_hud_backing(st->lbl_tide_speed);
    lv_obj_add_flag(st->lbl_tide_speed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(st->lbl_tide_speed, CX - 30, CY + 76);

    // Wind indices: T (true, wind_true) below A (apparent, warn) in z-order.
    st->twa_marker = make_wind_marker(st, r, "T", theme.wind_true);
    st->awa_marker = make_wind_marker(st, r, "A", theme.warn);

    build_bezel(st, r);
    build_cardinals(st, r);
    build_waypoint(st, r);

    // Centre hero = AWS (apparent wind speed), web-design layout: a small dim
    // "AWS" caption directly above the big amber/theme.warn number. Heading is
    // already shown by the rotated bezel + red bow lubber, so HDG is demoted to a
    // tiny dim readout below the AWS number (kept for at-a-glance reference).
    lv_obj_t *awscap = lv_label_create(r);
    lv_label_set_text(awscap, "AWS");
    lv_obj_set_style_text_font(awscap, &font_b612_14, 0);
    lv_obj_set_style_text_color(awscap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(awscap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(awscap, 80);
    lv_obj_set_pos(awscap, CX - 40, CY - 44);

    st->lbl_aws_value = lv_label_create(r);
    lv_label_set_text(st->lbl_aws_value, "--");
    lv_obj_set_style_text_font(st->lbl_aws_value, &font_xl_64, 0);
    lv_obj_set_style_text_color(st->lbl_aws_value, lv_color_hex(hero_rgb), 0);
    lv_obj_set_style_text_align(st->lbl_aws_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_aws_value, 200);
    lv_obj_set_pos(st->lbl_aws_value, CX - 100, CY - 30);

    // Tiny dim HDG readout under the AWS hero (heading is primarily the bezel).
    st->lbl_hdg_value = lv_label_create(r);
    lv_label_set_text(st->lbl_hdg_value, "--\xC2\xB0");
    lv_obj_set_style_text_font(st->lbl_hdg_value, &font_b612_14, 0);
    lv_obj_set_style_text_color(st->lbl_hdg_value, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(st->lbl_hdg_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_hdg_value, 100);
    lv_obj_set_pos(st->lbl_hdg_value, CX - 50, CY + 40);

    // Bottom numeric tiles: fixed band pinned to the bottom (TILE_BAND tall),
    // matching the rose area reserved above. Dropped on a short steering rect
    // (show_band == false) so the dial gets the full height.
    if (st->show_band) build_tiles(st, r, w, h, TILE_BAND);

    lv_obj_set_user_data(st->root, st);
    // Freed on teardown: MIDL screens are rebuilt live (theme switch / push).
    lv_obj_add_event_cb(st->root, free_user_data_cb, LV_EVENT_DELETE, nullptr);
    return st;
}

static void update(WindDialState *st, const boat::View &d) {
    if (!st) return;
    char buf[64];

    // --- AWS centre hero (apparent wind speed, the dominant number) ---
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        ui::set_text_if_changed(st->lbl_aws_value, st->last_aws, sizeof(st->last_aws), buf);
    } else {
        ui::set_text_if_changed(st->lbl_aws_value, st->last_aws, sizeof(st->last_aws), "--");
    }
    // --- TWS band tile (null-safe: dropped on a short steering rect) ---
    if (!isnan(d.tws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.tws));
        ui::set_text_if_changed(st->tile_tws, st->last_tws, sizeof(st->last_tws), buf);
    } else {
        ui::set_text_if_changed(st->tile_tws, st->last_tws, sizeof(st->last_tws), "--");
    }

    // --- AWA: angle tile + marker rotation ---
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        ui::set_text_if_changed(st->tile_awa, st->last_awa, sizeof(st->last_awa), buf);
        ui::set_rot_if_changed(st->awa_marker, &st->last_awa_rot, deg_to_lvgl(deg));
        ui::set_hidden_if_changed(st->awa_marker, &st->last_awa_hidden, false);
    } else {
        ui::set_text_if_changed(st->tile_awa, st->last_awa, sizeof(st->last_awa), "--");
        ui::set_hidden_if_changed(st->awa_marker, &st->last_awa_hidden, true);
    }

    // --- TWA ---
    if (!isnan(d.twa)) {
        double deg = rad_to_deg_pos(d.twa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        ui::set_text_if_changed(st->tile_twa, st->last_twa, sizeof(st->last_twa), buf);
        ui::set_rot_if_changed(st->twa_marker, &st->last_twa_rot, deg_to_lvgl(deg));
        ui::set_hidden_if_changed(st->twa_marker, &st->last_twa_hidden, false);
    } else {
        ui::set_text_if_changed(st->tile_twa, st->last_twa, sizeof(st->last_twa), "--");
        ui::set_hidden_if_changed(st->twa_marker, &st->last_twa_hidden, true);
    }

    // --- bezel rotation (heading) + cardinal layout + HDG digits ---
    double hdg_deg = NAN;
    if (!isnan(d.headingTrue)) hdg_deg = rad_to_deg_pos(d.headingTrue);
    double card_hdg = isnan(hdg_deg) ? 0.0 : hdg_deg;
    int16_t bez_rot = deg_to_lvgl(-card_hdg);
    if (bez_rot != st->last_bezel_rot) {
        st->last_bezel_rot = bez_rot;
        lv_obj_set_style_transform_rotation(st->bezel, bez_rot, 0);
        layout_cardinals(st, card_hdg);
    }
    if (!isnan(hdg_deg)) {
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", hdg_deg);
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg), buf);
    } else {
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg),
                                "--\xC2\xB0");
    }

    // --- tide / current vector ---
    bool have_current = !isnan(d.currentSetTrue) && !isnan(d.currentDrift) && !isnan(hdg_deg);
    bool flowing = have_current && d.currentDrift > 0.05;
    if (flowing) {
        double tide_rel = rad_to_deg_pos(d.currentSetTrue) - hdg_deg;
        while (tide_rel < 0)
            tide_rel += 360;
        ui::set_rot_if_changed(st->tide_arrow, &st->last_tide_rot, deg_to_lvgl(tide_rel));
        ui::set_hidden_if_changed(st->tide_arrow, &st->last_tide_hidden, false);
        ui::set_hidden_if_changed(st->tide_zero, &st->last_tide_zero_hidden, true);
        snprintf(buf, sizeof(buf), "%.1fkn", mps_to_kn(d.currentDrift));
        ui::set_text_if_changed(st->lbl_tide_speed, st->last_tide, sizeof(st->last_tide), buf);
        ui::set_hidden_if_changed(st->lbl_drift_cap, &st->last_drift_cap_hidden, false);
        ui::set_hidden_if_changed(st->lbl_tide_speed, &st->last_tide_speed_hidden, false);
    } else {
        ui::set_hidden_if_changed(st->tide_arrow, &st->last_tide_hidden, true);
        ui::set_hidden_if_changed(st->tide_zero, &st->last_tide_zero_hidden, !have_current);
        ui::set_text_if_changed(st->lbl_tide_speed, st->last_tide, sizeof(st->last_tide),
                                have_current ? "0.0kn" : "");
        ui::set_hidden_if_changed(st->lbl_drift_cap, &st->last_drift_cap_hidden, !have_current);
        ui::set_hidden_if_changed(st->lbl_tide_speed, &st->last_tide_speed_hidden, !have_current);
    }

    // --- waypoint pip ---
    if (!isnan(d.btw) && !isnan(hdg_deg)) {
        double wp_rel = rad_to_deg_pos(d.btw) - hdg_deg;
        while (wp_rel < 0)
            wp_rel += 360;
        ui::set_rot_if_changed(st->waypoint_marker, &st->last_wp_rot, deg_to_lvgl(wp_rel));
        ui::set_hidden_if_changed(st->waypoint_marker, &st->last_wp_hidden, false);
    } else {
        ui::set_hidden_if_changed(st->waypoint_marker, &st->last_wp_hidden, true);
    }
}

}  // namespace winddial

// ---------------------------------------------------------------------------
// Full-screen autopilot HUD (autopilot element on a large/full-screen leaf).
//
// Ported from the retired C++ screen_autopilot.cpp. The legacy screen uses ~10 file-static
// lv_obj_t* handles + a mode modal + touch/nudge wiring; here all HUD state lives
// in a PSRAM-allocated ApHudState attached to the HUD root's user_data, so the
// element can be instantiated more than once without file-scope state. The HUD
// reads boat::View directly (NOT through MetricBinding), like the legacy screen.
//
// Crucially this composite renders ONLY the dial + readouts + mode badge — NOT
// the nudge buttons. A composite element can't carry per-button actions, so the
// autopilot SCREEN is authored as a col-split: this leaf over a row of `button`
// leaves whose action.kind=command dispatches "autopilot heading ±N" (exactly
// like the steering / wind_steer screens). The reusable glass-cockpit parts
// (semicircular HEADING-UP ui::build_compass, HDG/COG/CTS/AP-target
// ui::build_marker_ring, ui::build_xte_strip, ui::numeric_tile) are shared with
// the legacy screen; only the refresh logic is ported here. Geometry is derived
// from the passed (w, h) of the tile rect (≈ 480x480), centered locally — NOT
// LCD_W/LCD_H — so positions stay tile-relative.

struct ApHudState {
    lv_obj_t *root;
    ui::Compass cp;
    ui::XteStrip xte;  // repurposed as a RUDDER center-zero strip (PORT..STBD, ±35°)
    lv_obj_t *lbl_mode;
    lv_obj_t *lbl_hdg_value;
    lv_obj_t *lbl_cogsog;
    lv_obj_t *tile_depth, *tile_speed, *tile_aws, *tile_awa;

    // Engage chip (top-left): Auto/STBY toggle wired DIRECTLY to a SignalK state
    // PUT (NOT the permission-gated `autopilot mode` command path). lbl_engage is
    // the chip's text label; s_last_engage is the mode to re-engage from STBY.
    lv_obj_t *btn_engage, *lbl_engage;
    char s_last_engage[8];
    lv_obj_t *lbl_target;  // "TGT <ddd>°" caption near the HDG hero (hidden on NaN)

    // Dirty caches (mirror screen_autopilot.cpp's file-statics).
    char last_mode[16];
    uint32_t last_mode_color;
    uint32_t engaged_rgb;  // engaged badge colour (style.color, else theme.good)
    char last_hdg[16];
    char last_cogsog[48];
    char last_depth[12];
    char last_speed[12];
    char last_aws[12];
    char last_awa[12];
    int16_t last_scale_rot;
    int last_xte_x;  // last rudder needle x
    char last_xte_txt[16];
    char last_engage[8];
    uint32_t last_engage_color;
    char last_target[12];
};

namespace aphud {

// PUT steering/autopilot/state DIRECTLY (mirrors screen_autopilot.cpp put_state),
// NOT the `autopilot mode` command funnel: autopilot::set_mode is permission-gated
// (allow_engage defaults OFF), so the command path is silently Forbidden on a stock
// device. The legacy HUD engages by PUTting the state string verbatim; do the same.
static void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering.autopilot.state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[aphud] state -> %s queued", state);
}

// Engage chip handler. Runs on the UI task: read boat::View (copyData), decide
// engaged, then post the state PUT to the net worker (non-blocking). Toggling
// from STBY re-engages the last steering mode (st->s_last_engage); toggling from
// any engaged mode drops to standby.
static void on_engage_short(lv_event_t *e) {
    ApHudState *st = (ApHudState *)lv_event_get_user_data(e);
    if (!st) return;
    boat::View d;
    boat::current_view(d);
    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;
    put_state(engaged ? "standby" : st->s_last_engage);
}

// HOME chip handler: post ShowScreen "home" (the index defaultScreen) (mirrors tile_clicked_cb).
static void on_home(lv_event_t *e) {
    (void)e;
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, "home", sizeof(c.a) - 1);  // index defaultScreen
    c.t_post_us = micros();
    app::post(c, 0);
}

// Build the HUD into `parent` (the tile root) filling its w x h content area.
// Returns a PSRAM ApHudState* (also attached to the HUD root's user_data).
// `engaged_rgb`: the engaged mode badge colour (style.color, else theme.good).
static ApHudState *build(lv_obj_t *parent, int w, int h, uint32_t engaged_rgb) {
    ApHudState *st = (ApHudState *)heap_caps_calloc(1, sizeof(ApHudState), MALLOC_CAP_SPIRAM);
    if (!st) {
        net::logf("[layout] ap hud alloc failed");
        return nullptr;
    }
    st->engaged_rgb = engaged_rgb;
    // Sentinel-init the dirty caches (calloc gives 0; force "unset").
    st->last_mode[0] = (char)0xFF;
    st->last_mode_color = 0xFFFFFFFF;
    st->last_hdg[0] = (char)0xFF;
    st->last_cogsog[0] = (char)0xFF;
    st->last_depth[0] = (char)0xFF;
    st->last_speed[0] = (char)0xFF;
    st->last_aws[0] = (char)0xFF;
    st->last_awa[0] = (char)0xFF;
    st->last_scale_rot = INT16_MIN;
    st->last_xte_x = INT16_MIN;
    st->last_xte_txt[0] = (char)0xFF;
    st->last_engage[0] = (char)0xFF;
    st->last_engage_color = 0xFFFFFFFF;
    st->last_target[0] = (char)0xFF;
    strcpy(st->s_last_engage, "auto");  // default mode to re-engage from STBY

    // HUD root: transparent, borderless, no-pad container filling the tile so its
    // local (0..w / 0..h) coordinates line up with the rect and update_* can find
    // it by user_data. (Tile chrome is already suppressed by the caller.)
    st->root = lv_obj_create(parent);
    lv_obj_set_size(st->root, w, h);
    lv_obj_set_pos(st->root, 0, 0);
    lv_obj_set_style_bg_opa(st->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st->root, 0, 0);
    lv_obj_set_style_radius(st->root, 0, 0);
    lv_obj_set_style_pad_all(st->root, 0, 0);
    lv_obj_clear_flag(st->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st->root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *r = st->root;

    // --- vertical band budget -------------------------------------------------
    // This HUD can land in a SHORT leaf (e.g. a [4,1] col-split gives the
    // autopilot leaf ~480x384, NOT 480x480). Lay out from the REAL `h`, never an
    // assumed 480, and reserve explicit non-overlapping bands so nothing
    // collides:
    //   [ badge ][ compass region ][ COG/SOG ][ XTE ][ tile band? ]
    // The compass radius is the slack: it gets whatever vertical space is left
    // after the fixed bands, capped by width. On a short leaf the secondary
    // numeric tiles are dropped so the dial + HDG + COG/SOG + XTE stay legible.
    const int top_bar_h = 44;  // top mode badge
    const int cogsog_h = 28;   // COG/SOG sub-line strip (own reserved band)
    const int xte_h = 44;      // cross-track-error strip
    const int gap = 8;
    const int n = 4;
    const int sq = (w - gap * (n + 1)) / n;  // numeric-tile square side
    const int tile_band_h = sq + gap;        // tiles + bottom gap
    // Drop the secondary numeric tiles when the leaf is too short to fit them
    // under the dial+COG/SOG+XTE without squeezing. The steering essentials
    // (compass, HDG, COG/SOG, XTE) take priority on a short autopilot leaf.
    const bool show_tiles = (h >= 440);
    const int reserved_below = cogsog_h + xte_h + (show_tiles ? tile_band_h : 0);

    // --- mode badge (top-mid): a filled pill (theme.good) when engaged, hollow
    // outline when standby. Built as a small rounded container with the mode label
    // centered, so the engaged/standby state reads at a glance even from across
    // the cockpit. The badge bg/border are recolored in update().
    lv_obj_t *badge = lv_obj_create(r);
    lv_obj_set_size(badge, 130, 32);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(badge, 16, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    st->lbl_mode = lv_label_create(badge);
    lv_label_set_text(st->lbl_mode, "STANDBY");
    lv_obj_set_style_text_font(st->lbl_mode, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_mode, lv_color_hex(theme.fg_dim), 0);
    lv_obj_center(st->lbl_mode);

    // --- engage chip (top-LEFT): Auto/STBY toggle. A composite tile can't carry
    // MIDL button actions, so this is wired DIRECTLY (on_engage_short -> direct
    // state PUT), exactly like the legacy screen_autopilot.cpp ON/STBY chip. ~110x40
    // rounded button with a label; the label text/color are state-driven in update.
    st->btn_engage = lv_button_create(r);
    lv_obj_set_size(st->btn_engage, 110, 40);
    lv_obj_align(st->btn_engage, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(st->btn_engage, ui::chrome::panel_radius, 0);
    lv_obj_set_style_bg_color(st->btn_engage, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(st->btn_engage, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(st->btn_engage, ui::chrome::panel_border, 0);
    lv_obj_set_style_pad_all(st->btn_engage, 0, 0);
    st->lbl_engage = lv_label_create(st->btn_engage);
    lv_label_set_text(st->lbl_engage, "ON");
    lv_obj_set_style_text_font(st->lbl_engage, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_engage, lv_color_hex(theme.fg), 0);
    lv_obj_center(st->lbl_engage);
    lv_obj_add_event_cb(st->btn_engage, on_engage_short, LV_EVENT_SHORT_CLICKED, st);

    // --- HOME chip (top-RIGHT): balances the engage chip, posts ShowScreen. ---
    lv_obj_t *btn_home = lv_button_create(r);
    lv_obj_set_size(btn_home, 110, 40);
    lv_obj_align(btn_home, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(btn_home, ui::chrome::panel_radius, 0);
    lv_obj_set_style_bg_color(btn_home, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(btn_home, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(btn_home, ui::chrome::panel_border, 0);
    lv_obj_set_style_pad_all(btn_home, 0, 0);
    lv_obj_t *lbl_home = lv_label_create(btn_home);
    lv_label_set_text(lbl_home, "HOME");
    lv_obj_set_style_text_font(lbl_home, &font_b612_20, 0);
    lv_obj_set_style_text_color(lbl_home, lv_color_hex(theme.fg), 0);
    lv_obj_center(lbl_home);
    lv_obj_add_event_cb(btn_home, on_home, LV_EVENT_SHORT_CLICKED, st);

    // --- semicircular HEADING-UP compass (rotating scale by -heading) ---
    // Compass region height = h - badge - everything reserved below it. Then
    // derive the dial WIDTH that yields that region height: build_compass gives
    // cp.h = r + 24 and r = cw/2 - 8, so cw = 2*(region_h - 24 + 8). Cap cw by
    // the leaf width (never exceed w - 32) so wide-but-short leaves stay
    // width-bound, and floor the region so a tiny leaf still draws a usable arc.
    int coy = top_bar_h;
    int region_h = h - top_bar_h - reserved_below;
    if (region_h < 120) region_h = 120;
    int cw_fit = 2 * (region_h - 24 + 8);  // width that makes cp.h == region_h
    int cw = w - 32;
    if (cw_fit < cw) cw = cw_fit;
    if (cw < 120) cw = 120;
    int cox = (w - cw) / 2;
    st->cp = ui::build_compass(r, cox, coy, cw);
    int scx = cox + st->cp.cx;
    int scy = coy + st->cp.cy;
    int compass_bottom = coy + st->cp.h;  // baseline of the compass region

    // --- center readouts (over the dial face) ---
    lv_obj_t *cap = lv_label_create(r);
    lv_label_set_text(cap, "HDG");
    lv_obj_set_style_text_font(cap, &font_b612_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, 120);
    lv_obj_set_pos(cap, scx - 60, scy - st->cp.r / 2 - 30);

    st->lbl_hdg_value = lv_label_create(r);
    lv_label_set_text(st->lbl_hdg_value, "--\xC2\xB0");
    // Scale the HDG hero to the dial radius so it never overlaps the rotating scale
    // on a compressed leaf (e.g. wind_steer, where the field band shrinks the dial).
    // A full-size dial keeps font_xl_64 (the <140 guard is false for aphud's region).
    const lv_font_t *hf = &font_xl_64;
    if (st->cp.r < 140) hf = &font_b612_48;
    if (st->cp.r < 110) hf = &font_b612_38;
    if (st->cp.r < 85) hf = &font_b612_28;
    lv_obj_set_style_text_font(st->lbl_hdg_value, hf, 0);
    lv_obj_set_style_text_color(st->lbl_hdg_value, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(st->lbl_hdg_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_hdg_value, 240);
    lv_obj_set_pos(st->lbl_hdg_value, scx - 120, scy - st->cp.r / 2 + 2);

    // "TGT <ddd>°" caption directly under the HDG hero, in the amber AP-target
    // color so it reads as a pair with the amber target bug on the rim. Hidden
    // until apTargetHdg is a real bearing (set/cleared in update()).
    st->lbl_target = lv_label_create(r);
    lv_label_set_text(st->lbl_target, "");
    lv_obj_set_style_text_font(st->lbl_target, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_target, lv_color_hex(theme.warn), 0);
    lv_obj_set_style_text_align(st->lbl_target, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_target, 160);
    lv_obj_set_pos(st->lbl_target, scx - 80, scy - st->cp.r / 2 + 2 + 64);
    lv_obj_add_flag(st->lbl_target, LV_OBJ_FLAG_HIDDEN);

    // COG/SOG sub-line: its OWN reserved strip directly under the compass
    // baseline (not floating over the dial face at scy), so it never overlaps the
    // XTE strip or the tile band below it.
    int cogsog_y = compass_bottom + (cogsog_h - 20) / 2;  // vertically centre the 20px font
    st->lbl_cogsog = lv_label_create(r);
    lv_label_set_text(st->lbl_cogsog, "COG --\xC2\xB0  |  SOG -- kn");
    lv_obj_set_style_text_font(st->lbl_cogsog, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_cogsog, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(st->lbl_cogsog, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_cogsog, w);
    lv_obj_set_pos(st->lbl_cogsog, 0, cogsog_y);

    // --- HDG/COG/CTS + AP-target marker ring ---
    // Built on the HUD root (NOT the compass root, which is sized exactly to the
    // dial and would clip glyphs orbiting its top/sides), centered at the
    // compass's HUD-local center (scx, scy). The reference is HDG (heading-up),
    // so the HDG marker rides at offset 0 under the red lubber. occlude_lower
    // hides the bottom half (it would overlap the XTE strip / tiles). Built AFTER
    // the center readouts so it draws on top. glyph/filled/color are baked here;
    // bearings fill in update.
    ui::MarkerSpec ap_markers[4] = {
        {NAN, ui::Glyph::Triangle, true, theme.accent},  // HDG (under the red lubber at offset 0)
        {NAN, ui::Glyph::Triangle, false, theme.good},   // COG
        {NAN, ui::Glyph::Diamond, true, theme.alarm},    // CTS
        {NAN, ui::Glyph::Diamond, true, theme.warn},     // AP target (amber bug), hidden until set
    };
    st->cp.markers = ui::build_marker_ring(r, scx, scy, st->cp.r - ui::kSemiMarkerInset, ap_markers,
                                           4, /*occlude_lower=*/true);

    // --- RUDDER strip: its own band directly under the COG/SOG strip ---
    // Center-zero helm-position indicator (the relevant AP feedback), replacing
    // the old XTE strip. PORT…STBD, ±35° full-scale, cyan (accent) needle — red is
    // reserved for alarm/port cues. Caption added below as "RUDDER".
    int xte_y = compass_bottom + cogsog_h;
    st->xte = ui::build_centerzero_strip(r, 16, xte_y, w - 32, xte_h, "PORT", "STBD",
                                         /*full_scale=*/35.0, /*tick_decimals=*/0, theme.accent);
    lv_obj_t *rud_cap = lv_label_create(r);
    lv_label_set_text(rud_cap, "RUDDER");
    lv_obj_set_style_text_font(rud_cap, &font_b612_14, 0);
    lv_obj_set_style_text_color(rud_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(rud_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(rud_cap, w);
    // Sits in the slack below the strip band (tiles, if any, are bottom-pinned far
    // below). Centered under the helm bar.
    lv_obj_set_pos(rud_cap, 0, xte_y + xte_h);

    // --- numeric tiles (square, pinned to the bottom) ---
    // Dropped entirely on a short leaf (show_tiles == false): the compass + HDG +
    // COG/SOG + XTE are the steering priority, and squeezing 4 tiles under them
    // would collide. The pointers stay NULL (calloc) and update() is null-safe.
    if (show_tiles) {
        const lv_font_t *tv = &font_b612_38;
        int ty = h - sq - gap;  // bottom row, below the XTE band
        st->tile_depth = ui::numeric_tile(r, gap, ty, sq, sq, "DEPTH", "m", tv, theme.fg);
        st->tile_speed = ui::numeric_tile(r, gap * 2 + sq, ty, sq, sq, "STW", "kn", tv, theme.fg);
        st->tile_aws =
            ui::numeric_tile(r, gap * 3 + sq * 2, ty, sq, sq, "AWS", "kn", tv, theme.warn);
        st->tile_awa = ui::numeric_tile(r, gap * 4 + sq * 3, ty, sq, sq, "AWA", "", tv, theme.fg);
    }

    lv_obj_set_user_data(st->root, st);
    // Freed on teardown: MIDL screens are rebuilt live (theme switch / push).
    lv_obj_add_event_cb(st->root, free_user_data_cb, LV_EVENT_DELETE, nullptr);
    return st;
}

static void update(ApHudState *st, const boat::View &d) {
    if (!st) return;
    char buf[64];

    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;

    // Mode badge: uppercased apState; filled pill (theme.good bg) when engaged,
    // hollow outline (transparent bg, panel_edge border) when standby. The label
    // text stays high-contrast either way.
    if (d.apState[0]) {
        char up[16];
        size_t i = 0;
        for (; d.apState[i] && i < sizeof(up) - 1; ++i)
            up[i] = toupper((unsigned char)d.apState[i]);
        up[i] = 0;
        ui::set_text_if_changed(st->lbl_mode, st->last_mode, sizeof(st->last_mode), up);
    } else {
        ui::set_text_if_changed(st->lbl_mode, st->last_mode, sizeof(st->last_mode), "OFFLINE");
    }
    // The badge text color is cached on last_mode_color; recolor the pill
    // container (parent of the label) only when that cache reports a flip, so the
    // bg/border style writes track the same engaged transition without their own
    // cache field.
    uint32_t prev_mode_color = st->last_mode_color;
    ui::set_text_color_if_changed(st->lbl_mode, &st->last_mode_color,
                                  engaged ? badge_ink(st->engaged_rgb) : theme.fg_dim);
    if (st->last_mode_color != prev_mode_color) {
        lv_obj_t *badge = lv_obj_get_parent(st->lbl_mode);
        if (badge) {
            lv_obj_set_style_bg_opa(badge, engaged ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(st->engaged_rgb), 0);
            lv_obj_set_style_border_color(
                badge, lv_color_hex(engaged ? st->engaged_rgb : theme.panel_edge), 0);
        }
    }

    // Heading: big value + rotate the compass tick ring by -heading and
    // reposition the upright degree labels to match (north-up when no heading).
    double hdg_deg = NAN;
    if (!isnan(d.headingTrue)) {
        hdg_deg = rad_to_deg_pos(d.headingTrue);
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", hdg_deg);
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg), buf);
    } else {
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg),
                                "--\xC2\xB0");
    }
    double label_hdg = isnan(hdg_deg) ? 0.0 : hdg_deg;
    int16_t scale_rot = winddial::deg_to_lvgl(-label_hdg);
    if (scale_rot != st->last_scale_rot) {
        st->last_scale_rot = scale_rot;
        lv_obj_set_style_transform_rotation(st->cp.scale, scale_rot, 0);
        ui::compass_layout_labels(st->cp, label_hdg);
    }

    // Marker ring: HDG/COG/CTS + AP target. Reference is HDG (heading-up, matching
    // the rotating scale), so the HDG marker rides at offset 0 under the lubber.
    // The target uses the AP's reported target heading; NaN hides any marker.
    double hdg_b = hdg_deg;
    double cog_b = isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    double cts_b = isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    double tgt_b = isnan(d.apTargetHdg) ? NAN : rad_to_deg_pos(d.apTargetHdg);
    ui::MarkerSpec live[4] = {
        {hdg_b, ui::Glyph::Triangle, true, theme.accent},
        {cog_b, ui::Glyph::Triangle, false, theme.good},
        {cts_b, ui::Glyph::Diamond, true, theme.alarm},
        {tgt_b, ui::Glyph::Diamond, true, theme.warn},
    };
    double ref = isnan(hdg_b) ? 0.0 : hdg_b;
    ui::marker_ring_update(st->cp.markers, live, 4, ref);

    // Engage chip: "STBY" (drop to standby) when engaged, "ON" (engage) when
    // standby. Filled green when engaged, hollow outline when standby — mirrors
    // the badge's engaged cue so the chip and badge agree at a glance.
    ui::set_text_if_changed(st->lbl_engage, st->last_engage, sizeof(st->last_engage),
                            engaged ? "STBY" : "ON");
    uint32_t want_engage_color = engaged ? theme.good : theme.panel_edge;
    if (want_engage_color != st->last_engage_color) {
        st->last_engage_color = want_engage_color;
        lv_obj_set_style_bg_opa(st->btn_engage, engaged ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(st->btn_engage, lv_color_hex(theme.good), 0);
        lv_obj_set_style_border_color(st->btn_engage, lv_color_hex(want_engage_color), 0);
        lv_obj_set_style_text_color(st->lbl_engage, lv_color_hex(engaged ? theme.bg : theme.fg), 0);
    }

    // TGT caption: "TGT <ddd>°" near the HDG hero in the amber target color, hidden
    // when the AP has no target bearing (NaN).
    if (st->lbl_target) {
        if (isnan(tgt_b)) {
            if (st->last_target[0] != (char)0xFF) {
                st->last_target[0] = (char)0xFF;  // sentinel: forces re-show when target returns
                lv_obj_add_flag(st->lbl_target, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            char tbuf[12];
            snprintf(tbuf, sizeof(tbuf), "TGT %03.0f\xC2\xB0", tgt_b);
            if (ui::set_text_if_changed(st->lbl_target, st->last_target, sizeof(st->last_target),
                                        tbuf))
                lv_obj_clear_flag(st->lbl_target, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // COG / SOG sub-line.
    char cogs[16], sogs[16];
    if (!isnan(d.cogTrue))
        snprintf(cogs, sizeof(cogs), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
    else
        snprintf(cogs, sizeof(cogs), "--\xC2\xB0");
    if (!isnan(d.sog))
        snprintf(sogs, sizeof(sogs), "%.1f kn", mps_to_kn(d.sog));
    else
        snprintf(sogs, sizeof(sogs), "-- kn");
    snprintf(buf, sizeof(buf), "COG %s  |  SOG %s", cogs, sogs);
    ui::set_text_if_changed(st->lbl_cogsog, st->last_cogsog, sizeof(st->last_cogsog), buf);

    // RUDDER needle (helm position, the relevant AP feedback — replaces XTE).
    // steering.rudderAngle is radians; +ve = starboard. Map ±35° full-scale to
    // ±half_px deflection (clamped). Readout "<mag>° P/S" matches the rudder P/S
    // convention in format_metric (P for port/negative, S for starboard/positive).
    if (!isnan(d.rudder)) {
        double deg = d.rudder * 180.0 / M_PI;  // + = starboard
        if (deg > 35.0) deg = 35.0;
        if (deg < -35.0) deg = -35.0;
        int nx = st->xte.center_x + (int)(deg / 35.0 * st->xte.half_px) - 1;
        if (nx != st->last_xte_x) {
            st->last_xte_x = nx;
            lv_obj_set_x(st->xte.needle, nx);
        }
    }
    if (st->xte.value) {
        char xbuf[16];
        if (isnan(d.rudder)) {
            snprintf(xbuf, sizeof(xbuf), "--");
        } else {
            double deg = d.rudder * 180.0 / M_PI;
            double mag = fabs(deg);
            if (mag < 0.5)
                snprintf(xbuf, sizeof(xbuf), "0\xC2\xB0");
            else
                snprintf(xbuf, sizeof(xbuf), "%.0f\xC2\xB0 %c", mag, deg > 0 ? 'S' : 'P');
        }
        ui::set_text_if_changed(st->xte.value, st->last_xte_txt, sizeof(st->last_xte_txt), xbuf);
    }

    // Bottom tiles: DEPTH / STW / AWS / AWA.
    {
        vfmt::format_scaled(d.depth, config::format().depth, buf, sizeof(buf));
        ui::set_text_if_changed(st->tile_depth, st->last_depth, sizeof(st->last_depth), buf);
    }
    if (!isnan(d.stw)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.stw));
        ui::set_text_if_changed(st->tile_speed, st->last_speed, sizeof(st->last_speed), buf);
    } else {
        ui::set_text_if_changed(st->tile_speed, st->last_speed, sizeof(st->last_speed), "--");
    }
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        ui::set_text_if_changed(st->tile_aws, st->last_aws, sizeof(st->last_aws), buf);
    } else {
        ui::set_text_if_changed(st->tile_aws, st->last_aws, sizeof(st->last_aws), "--");
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        ui::set_text_if_changed(st->tile_awa, st->last_awa, sizeof(st->last_awa), buf);
    } else {
        ui::set_text_if_changed(st->tile_awa, st->last_awa, sizeof(st->last_awa), "--");
    }
}

}  // namespace aphud

// ---------------------------------------------------------------------------
// windsteer:: — semicircular HEADING-UP wind-steering composite.
//
// Sibling of aphud:: (same glass-cockpit building blocks: ui::build_compass,
// ui::build_marker_ring, center HDG hero, tile band, PSRAM state, short-leaf
// band-drop). DISTINCT from the `wind` screen (winddial:: — a full CIRCULAR
// rotating dial): this is heading-up and carries the LAYLINE sectors (no-go +
// target) ported from the retired C++ screen_wind_steer.cpp::refresh, plus a
// HDG/COG/CTS/TWD marker ring and AWS/AWA/TWS/TWA tiles. It reads boat::View
// directly (NOT through MetricBinding), like the legacy screen and aphud::.
//
// As with aphud::, the SCREEN authors the nudge/tack button row as sibling
// button leaves (col-split [3,1]); this leaf renders ONLY the dial + readouts +
// tiles. Geometry derives from the passed tile (w, h), centered locally.

struct WindSteerState {
    lv_obj_t *root;
    ui::Compass cp;
    lv_obj_t *lbl_mode;  // center mode badge (STANDBY/AUTO pill) — mirrors aphud
    lv_obj_t *lbl_hdg_value;
    lv_obj_t *lbl_sub;  // "TWA <mag>°S/P | TWD <ddd>°"
    // Layline sector arcs on the compass band.
    lv_obj_t *nogo, *target_a, *target_b;
    // Steering tile band: SOG / STW / VMG-wpt / VMG-wind.
    lv_obj_t *tile_sog, *tile_stw, *tile_vmg, *tile_vmgw;
    // Center-zero RUDDER strip (PORT..STBD, ±35°, cyan needle) — reuses
    // ui::build_centerzero_strip exactly like aphud::.
    ui::XteStrip rud;
    // Engage chip (top-left): Auto/STBY toggle wired DIRECTLY to a SignalK state
    // PUT (mirrors aphud::). s_last_engage is the mode to re-engage from STBY.
    lv_obj_t *btn_engage, *lbl_engage;
    char s_last_engage[8];

    // Dirty caches.
    char last_mode[16];
    uint32_t last_mode_color;
    uint32_t engaged_rgb;  // engaged badge colour (style.color, else theme.good)
    char last_hdg[16];
    char last_sub[40];
    char last_sog[12];
    char last_stw[12];
    char last_vmg[12];
    char last_vmgw[12];
    int last_rud_x;  // last rudder needle x
    char last_rud_txt[16];
    char last_engage[8];
    uint32_t last_engage_color;
    int16_t last_scale_rot;
    // Sector last-angle caches (LVGL deci-degrees: start*100 + end packed start/end).
    int last_nogo_s, last_nogo_e;
    int last_tgta_s, last_tgta_e;
    int last_tgtb_s, last_tgtb_e;
    bool last_sectors_hidden;
    bool last_target_dim;  // green bands dimmed (estimated beat/gybe) vs crisp
};

namespace windsteer {

static double norm360(double d) {
    while (d < 0)
        d += 360;
    while (d >= 360)
        d -= 360;
    return d;
}

// A colored sector arc on the compass band (no-go / target). Angles set live in
// update(). Mirrors screen_wind_steer.cpp::make_sector.
static lv_obj_t *make_sector(lv_obj_t *parent, int cx, int cy, int radius, int width,
                             uint32_t color) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    int d = radius * 2;
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - radius, cy - radius);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 250, 290);
    lv_arc_set_angles(a, 250, 290);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    return a;
}

// Place a sector spanning screen angles [t0,t1] (deg from top, +cw) in LVGL.
// Mirrors screen_wind_steer.cpp::set_sector but caches the last start/end so the
// arc is only re-stroked on change. Returns true if it changed.
static bool set_sector(lv_obj_t *a, double t0, double t1, int *last_s, int *last_e) {
    int s = (int)norm360(270 + t0);
    int e = (int)norm360(270 + t1);
    if (s == *last_s && e == *last_e) return false;
    *last_s = s;
    *last_e = e;
    lv_arc_set_bg_angles(a, s, e);
    lv_arc_set_angles(a, s, e);
    return true;
}

// HOME chip handler: post ShowScreen "home" (the index defaultScreen) (mirrors aphud::on_home).
static void on_home(lv_event_t *e) {
    (void)e;
    app::Command c;
    c.type = app::CommandType::ShowScreen;
    strncpy(c.a, "home", sizeof(c.a) - 1);  // index defaultScreen
    c.t_post_us = micros();
    app::post(c, 0);
}

// PUT steering/autopilot/state DIRECTLY (mirrors aphud::put_state), NOT the
// permission-gated `autopilot mode` funnel. The legacy wind-steer screen engaged
// in WIND mode; do the same here.
static void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering.autopilot.state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[windsteer] state -> %s queued", state);
}

// Engage chip handler. Runs on the UI task: read boat::View, decide engaged, then
// post the state PUT. Toggling from STBY re-engages st->s_last_engage ("wind");
// toggling from any engaged mode drops to standby. Mirrors aphud::on_engage_short.
static void on_engage_short(lv_event_t *e) {
    WindSteerState *st = (WindSteerState *)lv_event_get_user_data(e);
    if (!st) return;
    boat::View d;
    boat::current_view(d);
    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;
    put_state(engaged ? "standby" : st->s_last_engage);
}

// Build the wind-steering composite into `parent` (the tile root) filling its
// w x h content area. Returns a PSRAM WindSteerState* (also attached to the root
// user_data). Mirrors aphud::build's band budget exactly.
// `engaged_rgb`: the engaged mode badge colour (style.color, else theme.good).
static WindSteerState *build(lv_obj_t *parent, int w, int h, uint32_t engaged_rgb) {
    WindSteerState *st =
        (WindSteerState *)heap_caps_calloc(1, sizeof(WindSteerState), MALLOC_CAP_SPIRAM);
    if (!st) {
        net::logf("[layout] windsteer alloc failed");
        return nullptr;
    }
    st->engaged_rgb = engaged_rgb;
    // Sentinel-init the dirty caches (calloc gives 0; force "unset").
    st->last_mode[0] = (char)0xFF;
    st->last_mode_color = 0xFFFFFFFF;
    st->last_hdg[0] = (char)0xFF;
    st->last_sub[0] = (char)0xFF;
    st->last_sog[0] = (char)0xFF;
    st->last_stw[0] = (char)0xFF;
    st->last_vmg[0] = (char)0xFF;
    st->last_vmgw[0] = (char)0xFF;
    st->last_rud_x = INT16_MIN;
    st->last_rud_txt[0] = (char)0xFF;
    st->last_engage[0] = (char)0xFF;
    st->last_engage_color = 0xFFFFFFFF;
    st->last_scale_rot = INT16_MIN;
    strcpy(st->s_last_engage, "wind");  // default mode to re-engage from STBY
    st->last_nogo_s = st->last_nogo_e = INT32_MIN;
    st->last_tgta_s = st->last_tgta_e = INT32_MIN;
    st->last_tgtb_s = st->last_tgtb_e = INT32_MIN;
    st->last_sectors_hidden = false;  // force first hide/show to apply
    st->last_target_dim = false;

    // Composite root: transparent, borderless, no-pad container filling the tile
    // so its local (0..w / 0..h) coordinates line up with the rect.
    st->root = lv_obj_create(parent);
    lv_obj_set_size(st->root, w, h);
    lv_obj_set_pos(st->root, 0, 0);
    lv_obj_set_style_bg_opa(st->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st->root, 0, 0);
    lv_obj_set_style_radius(st->root, 0, 0);
    lv_obj_set_style_pad_all(st->root, 0, 0);
    lv_obj_clear_flag(st->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st->root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *r = st->root;

    // --- vertical band budget -------------------------------------------------
    //   [ top chip row ][ compass region ][ sub-line ][ RUDDER strip ][ RUDDER cap ][
    //   SOG/STW/VMG/VMGw band? ]
    // Unlike aphud (which simply DROPS its tile band below 440 px), the SOG / STW
    // / VMG / VMGw band is the point of the wind_steer screen, so we keep it on a
    // SHORT leaf by reserving a COMPACT band and letting the dial radius shrink to
    // whatever vertical space is left after the fixed bands. Only a genuinely tiny
    // leaf (h < 280) drops the band entirely; there the dial + HDG + TWA/TWD +
    // RUDDER are the steering essentials and a squeezed band would collide.
    const int top_bar_h = 44;  // engage + HOME chip row
    const int sub_h = 28;      // TWA/TWD sub-line strip (own reserved band)
    const int rud_h = 44;      // RUDDER center-zero strip (own reserved band)
    const int rud_cap_h = 18;  // "RUDDER" caption strip under the helm bar
    const int gap = 8;
    const int n = 4;
    const int band_tw = (w - gap * (n + 1)) / n;  // tile width (4 across)
    // Compact band: a fixed short tile height (NOT the full square `band_tw`,
    // which would be ~110 px at w=480 and crowd the dial off a 360 px leaf).
    // numeric_tile honours an explicit h, so the band reads as a low value strip.
    const int band_th = 72;                 // compact tile content height
    const int tile_band_h = band_th + gap;  // band + bottom gap
    // Keep the band on any non-tiny leaf; only drop it when the dial would have
    // no room left at all.
    const bool show_tiles = (h >= 280);
    const int reserved_below = sub_h + rud_h + rud_cap_h + (show_tiles ? tile_band_h : 0);

    // --- engage chip (top-LEFT): Auto/STBY toggle wired DIRECTLY to a state PUT
    // (on_engage_short), exactly like aphud::build. A composite tile can't carry a
    // MIDL button action, so this is wired here; text/color are state-driven in
    // update(). ---
    st->btn_engage = lv_button_create(r);
    lv_obj_set_size(st->btn_engage, 110, 40);
    lv_obj_align(st->btn_engage, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(st->btn_engage, ui::chrome::panel_radius, 0);
    lv_obj_set_style_bg_color(st->btn_engage, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(st->btn_engage, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(st->btn_engage, ui::chrome::panel_border, 0);
    lv_obj_set_style_pad_all(st->btn_engage, 0, 0);
    st->lbl_engage = lv_label_create(st->btn_engage);
    lv_label_set_text(st->lbl_engage, "ON");
    lv_obj_set_style_text_font(st->lbl_engage, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_engage, lv_color_hex(theme.fg), 0);
    lv_obj_center(st->lbl_engage);
    lv_obj_add_event_cb(st->btn_engage, on_engage_short, LV_EVENT_SHORT_CLICKED, st);

    // --- HOME chip (top-RIGHT): posts ShowScreen "home" ---
    lv_obj_t *btn_home = lv_button_create(r);
    lv_obj_set_size(btn_home, 110, 40);
    lv_obj_align(btn_home, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(btn_home, ui::chrome::panel_radius, 0);
    lv_obj_set_style_bg_color(btn_home, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_border_color(btn_home, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(btn_home, ui::chrome::panel_border, 0);
    lv_obj_set_style_pad_all(btn_home, 0, 0);
    lv_obj_t *lbl_home = lv_label_create(btn_home);
    lv_label_set_text(lbl_home, "HOME");
    lv_obj_set_style_text_font(lbl_home, &font_b612_20, 0);
    lv_obj_set_style_text_color(lbl_home, lv_color_hex(theme.fg), 0);
    lv_obj_center(lbl_home);
    lv_obj_add_event_cb(btn_home, on_home, LV_EVENT_SHORT_CLICKED, st);

    // --- mode badge (top-MID): identical to aphud::build so the two screens read
    // as the same instrument — a filled pill (theme.good) when engaged, hollow
    // outline when standby, with the uppercased autopilot mode centered. The
    // bg/border are recolored in update(); sits between the engage + HOME chips. ---
    lv_obj_t *badge = lv_obj_create(r);
    lv_obj_set_size(badge, 130, 32);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(badge, 16, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    st->lbl_mode = lv_label_create(badge);
    lv_label_set_text(st->lbl_mode, "STANDBY");
    lv_obj_set_style_text_font(st->lbl_mode, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_mode, lv_color_hex(theme.fg_dim), 0);
    lv_obj_center(st->lbl_mode);

    // --- semicircular HEADING-UP compass (rotating scale by -heading) ---
    // Compass region height = h - top - reserved below; derive dial width that
    // yields that region height (cp.h = r + 24, r = cw/2 - 8 -> cw =
    // 2*(region_h - 24 + 8)), capped by leaf width. Identical to aphud::build.
    int coy = top_bar_h;
    int region_h = h - top_bar_h - reserved_below;
    if (region_h < 120) region_h = 120;
    int cw_fit = 2 * (region_h - 24 + 8);
    int cw = w - 32;
    if (cw_fit < cw) cw = cw_fit;
    if (cw < 120) cw = 120;
    int cox = (w - cw) / 2;
    st->cp = ui::build_compass(r, cox, coy, cw);
    int scx = cox + st->cp.cx;
    int scy = coy + st->cp.cy;
    int compass_bottom = coy + st->cp.h;

    // --- layline sectors on the compass band (built on the compass ROOT so they
    // rotate/clip with the dial face). Faint red no-go tint + crisp green target
    // marks; then raise the scale ticks / degree numbers / lubber back above them
    // so the band numbers stay legible (mirrors screen_wind_steer.cpp). ---
    int band_r = st->cp.r - 10;  // outer edge of the white band
    st->nogo = make_sector(st->cp.root, st->cp.cx, st->cp.cy, band_r, 14, theme.alarm);
    lv_obj_set_style_arc_opa(st->nogo, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(st->nogo, LV_OPA_40, LV_PART_INDICATOR);
    st->target_a = make_sector(st->cp.root, st->cp.cx, st->cp.cy, band_r, 16, theme.good);
    st->target_b = make_sector(st->cp.root, st->cp.cx, st->cp.cy, band_r, 16, theme.good);
    lv_obj_move_foreground(st->cp.scale);
    for (int i = 0; i < 12; ++i)
        if (st->cp.nums[i]) lv_obj_move_foreground(st->cp.nums[i]);
    lv_obj_move_foreground(st->cp.lubber);

    // --- center readouts (over the dial face) ---
    lv_obj_t *cap = lv_label_create(r);
    lv_label_set_text(cap, "HDG");
    lv_obj_set_style_text_font(cap, &font_b612_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, 120);
    lv_obj_set_pos(cap, scx - 60, scy - st->cp.r / 2 - 30);

    st->lbl_hdg_value = lv_label_create(r);
    lv_label_set_text(st->lbl_hdg_value, "--\xC2\xB0");
    // Scale the HDG hero to the dial radius so it never overlaps the rotating scale
    // on a compressed leaf (e.g. wind_steer, where the field band shrinks the dial).
    // A full-size dial keeps font_xl_64 (the <140 guard is false for aphud's region).
    const lv_font_t *hf = &font_xl_64;
    if (st->cp.r < 140) hf = &font_b612_48;
    if (st->cp.r < 110) hf = &font_b612_38;
    if (st->cp.r < 85) hf = &font_b612_28;
    lv_obj_set_style_text_font(st->lbl_hdg_value, hf, 0);
    lv_obj_set_style_text_color(st->lbl_hdg_value, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(st->lbl_hdg_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_hdg_value, 240);
    lv_obj_set_pos(st->lbl_hdg_value, scx - 120, scy - st->cp.r / 2 + 2);

    // TWA/TWD sub-line: its OWN reserved strip directly under the compass
    // baseline (not floating over the dial face), so it never overlaps the tile
    // band below it.
    int sub_y = compass_bottom + (sub_h - 20) / 2;  // vertically centre the 20px font
    st->lbl_sub = lv_label_create(r);
    lv_label_set_text(st->lbl_sub, "TWA --\xC2\xB0 | TWD --\xC2\xB0");
    lv_obj_set_style_text_font(st->lbl_sub, &font_b612_20, 0);
    lv_obj_set_style_text_color(st->lbl_sub, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(st->lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(st->lbl_sub, w);
    lv_obj_set_pos(st->lbl_sub, 0, sub_y);

    // --- HDG/COG/CTS + amber TWD-bug marker ring ---
    // Built on the composite root (NOT the compass root, which is sized exactly
    // to the dial and would clip glyphs orbiting its top/sides), centered at the
    // compass's local center (scx, scy). Reference is HDG (heading-up), so the HDG
    // marker rides at offset 0 under the red lubber. occlude_lower hides the
    // bottom half. Built AFTER the center readouts so it draws on top.
    ui::MarkerSpec ws_markers[4] = {
        {NAN, ui::Glyph::Triangle, true, theme.accent},  // HDG (under the red lubber at offset 0)
        {NAN, ui::Glyph::Triangle, false, theme.good},   // COG
        {NAN, ui::Glyph::Diamond, true, theme.alarm},    // CTS
        {NAN, ui::Glyph::Diamond, true, theme.warn},     // TWD wind bug (amber)
    };
    st->cp.markers = ui::build_marker_ring(r, scx, scy, st->cp.r - ui::kSemiMarkerInset, ws_markers,
                                           4, /*occlude_lower=*/true);

    // --- RUDDER strip: its own band directly under the TWA/TWD sub-line ---
    // Center-zero helm-position indicator reused from aphud:: (the same
    // ui::build_centerzero_strip the AP HUD uses for RUDDER): PORT…STBD, ±35°
    // full-scale, cyan (accent) needle. Caption "RUDDER" below it.
    int rud_y = compass_bottom + sub_h;
    st->rud = ui::build_centerzero_strip(r, 16, rud_y, w - 32, rud_h, "PORT", "STBD",
                                         /*full_scale=*/35.0, /*tick_decimals=*/0, theme.accent);
    lv_obj_t *rud_cap = lv_label_create(r);
    lv_label_set_text(rud_cap, "RUDDER");
    lv_obj_set_style_text_font(rud_cap, &font_b612_14, 0);
    lv_obj_set_style_text_color(rud_cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(rud_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(rud_cap, w);
    lv_obj_set_pos(rud_cap, 0, rud_y + rud_h);

    // --- numeric tiles (compact, pinned to the bottom) --- SOG/STW/VMG/VMGwind ---
    // Steering-useful speed band (AWS center + TWA/TWD live on the dial). Unlike
    // aphud, this band stays on a short leaf: it's COMPACT (band_th tall, not a
    // full square) and the dial radius already shrank to make room (region_h
    // budget above). A 28px value font fits the short tile without clipping the
    // caption/unit; the dial is a touch smaller but the fields are the point. The
    // band is pinned to the very bottom, below the RUDDER caption strip.
    if (show_tiles) {
        const lv_font_t *tv = &font_b612_28;
        int ty = h - band_th - gap;  // bottom row, below the rudder band + caption
        st->tile_sog = ui::numeric_tile(r, gap, ty, band_tw, band_th, "SOG", "kn", tv, theme.fg);
        st->tile_stw =
            ui::numeric_tile(r, gap * 2 + band_tw, ty, band_tw, band_th, "STW", "kn", tv, theme.fg);
        st->tile_vmg = ui::numeric_tile(r, gap * 3 + band_tw * 2, ty, band_tw, band_th, "VMG", "kn",
                                        tv, theme.fg);
        st->tile_vmgw = ui::numeric_tile(r, gap * 4 + band_tw * 3, ty, band_tw, band_th, "VMGw",
                                         "kn", tv, theme.fg);
    }

    lv_obj_set_user_data(st->root, st);
    // Freed on teardown: MIDL screens are rebuilt live (theme switch / push).
    lv_obj_add_event_cb(st->root, free_user_data_cb, LV_EVENT_DELETE, nullptr);
    return st;
}

static void update(WindSteerState *st, const boat::View &d) {
    if (!st) return;
    char buf[64];

    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;

    // Mode badge: uppercased apState; filled pill (theme.good bg) when engaged,
    // hollow outline (transparent bg, panel_edge border) when standby. Identical to
    // aphud::update so the two heading-up screens agree at a glance.
    if (st->lbl_mode) {
        if (d.apState[0]) {
            char up[16];
            size_t i = 0;
            for (; d.apState[i] && i < sizeof(up) - 1; ++i)
                up[i] = toupper((unsigned char)d.apState[i]);
            up[i] = 0;
            ui::set_text_if_changed(st->lbl_mode, st->last_mode, sizeof(st->last_mode), up);
        } else {
            ui::set_text_if_changed(st->lbl_mode, st->last_mode, sizeof(st->last_mode), "OFFLINE");
        }
        uint32_t prev_mode_color = st->last_mode_color;
        ui::set_text_color_if_changed(st->lbl_mode, &st->last_mode_color,
                                      engaged ? badge_ink(st->engaged_rgb) : theme.fg_dim);
        if (st->last_mode_color != prev_mode_color) {
            lv_obj_t *badge = lv_obj_get_parent(st->lbl_mode);
            if (badge) {
                lv_obj_set_style_bg_opa(badge, engaged ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_style_bg_color(badge, lv_color_hex(st->engaged_rgb), 0);
                lv_obj_set_style_border_color(
                    badge, lv_color_hex(engaged ? st->engaged_rgb : theme.panel_edge), 0);
            }
        }
    }

    // Heading: big value + rotate the compass tick ring by -heading and reposition
    // the upright degree labels (north-up when no heading). Same as aphud::update.
    double hdg = isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
    if (!isnan(hdg)) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", hdg);
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg), buf);
    } else {
        ui::set_text_if_changed(st->lbl_hdg_value, st->last_hdg, sizeof(st->last_hdg),
                                "--\xC2\xB0");
    }
    double label_hdg = isnan(hdg) ? 0.0 : hdg;
    int16_t scale_rot = winddial::deg_to_lvgl(-label_hdg);
    if (scale_rot != st->last_scale_rot) {
        st->last_scale_rot = scale_rot;
        lv_obj_set_style_transform_rotation(st->cp.scale, scale_rot, 0);
        ui::compass_layout_labels(st->cp, label_hdg);
    }

    // Wind angles + TWD (ported from screen_wind_steer.cpp::refresh).
    double twa_raw = isnan(d.twa) ? NAN : rad_to_deg_pos(d.twa);
    double twa_mag = NAN;
    bool stbd = true;
    if (!isnan(twa_raw)) {
        stbd = twa_raw <= 180.0;
        twa_mag = stbd ? twa_raw : 360.0 - twa_raw;
    }
    double twd = (!isnan(hdg) && !isnan(twa_raw)) ? norm360(hdg + twa_raw) : NAN;

    // Sub-line: TWA + TWD.
    char twas[12], twds[12];
    if (!isnan(twa_mag))
        snprintf(twas, sizeof(twas), "%.0f\xC2\xB0%c", twa_mag, stbd ? 'S' : 'P');
    else
        snprintf(twas, sizeof(twas), "--\xC2\xB0");
    if (!isnan(twd))
        snprintf(twds, sizeof(twds), "%03.0f\xC2\xB0", twd);
    else
        snprintf(twds, sizeof(twds), "--\xC2\xB0");
    snprintf(buf, sizeof(buf), "TWA %s  |  TWD %s", twas, twds);
    ui::set_text_if_changed(st->lbl_sub, st->last_sub, sizeof(st->last_sub), buf);

    // --- layline display: no-go + target sectors, heading-relative ---
    // (ported from screen_wind_steer.cpp::refresh ~419-447). NaN fallbacks:
    // beatAngle NaN -> 45°, gybeAngle NaN -> 150°, tws-derived tol NaN -> 10°.
    // When beat/gybe are DEFAULTED (no real polar data), the green target bands
    // render DIMMER (OPA_40) to signal "estimated".
    bool wind_up = isnan(twa_mag) ? true : (twa_mag <= 90.0);
    double beat = isnan(d.beatAngle) ? NAN : rad_to_deg_pos(d.beatAngle);
    double gybe = isnan(d.gybeAngle) ? NAN : rad_to_deg_pos(d.gybeAngle);
    bool beat_estimated = isnan(beat);
    bool gybe_estimated = isnan(gybe);
    double opt = wind_up ? (beat_estimated ? 45.0 : beat) : (gybe_estimated ? 150.0 : gybe);
    bool target_estimated = wind_up ? beat_estimated : gybe_estimated;
    double tws_kn = isnan(d.tws) ? NAN : mps_to_kn(d.tws);
    double tol = isnan(tws_kn) ? 10.0 : 16.0 - 0.6 * tws_kn;  // TWS-responsive band
    if (tol < 4.0) tol = 4.0;
    if (tol > 16.0) tol = 16.0;

    if (!isnan(twd) && !isnan(hdg)) {
        double twd_rel = norm360(twd - hdg);                         // wind dir from the bow
        double badc = wind_up ? twd_rel : norm360(twd_rel + 180.0);  // heading to avoid
        double half = wind_up ? opt : (180.0 - opt);                 // no-go half width
        if (half < 4) half = 4;
        if (half > 88) half = 88;
        double c = badc > 180.0 ? badc - 360.0 : badc;  // map to -180..180 for the arc helper
        set_sector(st->nogo, c - half, c + half, &st->last_nogo_s, &st->last_nogo_e);
        set_sector(st->target_a, c + half, c + half + tol, &st->last_tgta_s, &st->last_tgta_e);
        set_sector(st->target_b, c - half - tol, c - half, &st->last_tgtb_s, &st->last_tgtb_e);
        // Dim the green target bands when the laylines are ESTIMATED (no polar
        // beat/gybe), crisp/full when real. Toggle only on change.
        if (target_estimated != st->last_target_dim) {
            st->last_target_dim = target_estimated;
            lv_opa_t topa = target_estimated ? LV_OPA_40 : LV_OPA_COVER;
            lv_obj_set_style_arc_opa(st->target_a, topa, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(st->target_a, topa, LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(st->target_b, topa, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(st->target_b, topa, LV_PART_INDICATOR);
        }
        if (st->last_sectors_hidden) {
            st->last_sectors_hidden = false;
            lv_obj_clear_flag(st->nogo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(st->target_a, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(st->target_b, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!st->last_sectors_hidden) {
        st->last_sectors_hidden = true;
        lv_obj_add_flag(st->nogo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(st->target_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(st->target_b, LV_OBJ_FLAG_HIDDEN);
    }

    // Steering marker ring -> HDG/COG/CTS + amber TWD bug (heading-up). Bearings
    // match the AP HUD; marker_ring_update hides any NaN marker.
    double hdg_b = hdg;
    double cog_b = isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    double cts_b = isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    ui::MarkerSpec steer_live[4] = {
        {hdg_b, ui::Glyph::Triangle, true, theme.accent},
        {cog_b, ui::Glyph::Triangle, false, theme.good},
        {cts_b, ui::Glyph::Diamond, true, theme.alarm},
        {twd, ui::Glyph::Diamond, true, theme.warn},
    };
    double wind_ref = isnan(hdg) ? 0.0 : hdg;
    ui::marker_ring_update(st->cp.markers, steer_live, 4, wind_ref);

    // Engage chip: "STBY" (drop to standby) when engaged, "ON" (engage) when
    // standby. Filled green when engaged, hollow outline when standby — mirrors
    // aphud::update's chip exactly.
    ui::set_text_if_changed(st->lbl_engage, st->last_engage, sizeof(st->last_engage),
                            engaged ? "STBY" : "ON");
    uint32_t want_engage_color = engaged ? theme.good : theme.panel_edge;
    if (want_engage_color != st->last_engage_color) {
        st->last_engage_color = want_engage_color;
        lv_obj_set_style_bg_opa(st->btn_engage, engaged ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(st->btn_engage, lv_color_hex(theme.good), 0);
        lv_obj_set_style_border_color(st->btn_engage, lv_color_hex(want_engage_color), 0);
        lv_obj_set_style_text_color(st->lbl_engage, lv_color_hex(engaged ? theme.bg : theme.fg), 0);
    }

    // RUDDER needle (helm position) — same mapping as aphud::update. rudder is
    // radians, +ve = starboard; map ±35° full-scale to ±half_px (clamped).
    if (!isnan(d.rudder)) {
        double deg = d.rudder * 180.0 / M_PI;  // + = starboard
        if (deg > 35.0) deg = 35.0;
        if (deg < -35.0) deg = -35.0;
        int nx = st->rud.center_x + (int)(deg / 35.0 * st->rud.half_px) - 1;
        if (nx != st->last_rud_x) {
            st->last_rud_x = nx;
            lv_obj_set_x(st->rud.needle, nx);
        }
    }
    if (st->rud.value) {
        char rbuf[16];
        if (isnan(d.rudder)) {
            snprintf(rbuf, sizeof(rbuf), "--");
        } else {
            double deg = d.rudder * 180.0 / M_PI;
            double mag = fabs(deg);
            if (mag < 0.5)
                snprintf(rbuf, sizeof(rbuf), "0\xC2\xB0");
            else
                snprintf(rbuf, sizeof(rbuf), "%.0f\xC2\xB0 %c", mag, deg > 0 ? 'S' : 'P');
        }
        ui::set_text_if_changed(st->rud.value, st->last_rud_txt, sizeof(st->last_rud_txt), rbuf);
    }

    // Tiles: SOG / STW / VMG (to waypoint) / VMGw (wind/polar VMG). Null-safe —
    // dropped on a short leaf.
    if (st->tile_sog) {
        if (!isnan(d.sog)) {
            snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.sog));
            ui::set_text_if_changed(st->tile_sog, st->last_sog, sizeof(st->last_sog), buf);
        } else {
            ui::set_text_if_changed(st->tile_sog, st->last_sog, sizeof(st->last_sog), "--");
        }
    }
    if (st->tile_stw) {
        if (!isnan(d.stw)) {
            snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.stw));
            ui::set_text_if_changed(st->tile_stw, st->last_stw, sizeof(st->last_stw), buf);
        } else {
            ui::set_text_if_changed(st->tile_stw, st->last_stw, sizeof(st->last_stw), "--");
        }
    }
    if (st->tile_vmg) {
        if (!isnan(d.vmg)) {
            snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.vmg));
            ui::set_text_if_changed(st->tile_vmg, st->last_vmg, sizeof(st->last_vmg), buf);
        } else {
            ui::set_text_if_changed(st->tile_vmg, st->last_vmg, sizeof(st->last_vmg), "--");
        }
    }
    if (st->tile_vmgw) {
        if (!isnan(d.vmgWind)) {
            snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.vmgWind));
            ui::set_text_if_changed(st->tile_vmgw, st->last_vmgw, sizeof(st->last_vmgw), buf);
        } else {
            ui::set_text_if_changed(st->tile_vmgw, st->last_vmgw, sizeof(st->last_vmgw), "--");
        }
    }
}

}  // namespace windsteer

// Wind rose: dashed warn ring with center AWS value. Mirrors editor
// .wpreview .rose - small visual stand-in; the fullscreen wind dial
// (screen_wind.cpp) is the high-fidelity render.
// Full-screen detection: a single-leaf windrose screen lands a tile rect ≈ the
// whole 480x480 panel; the wind_steer screen gives the dial a near-full-width
// region (≈480x360, the rest a button row). Above this threshold we render the
// high-fidelity rotating dial (winddial::build). Dash (2x2 → ≈236x210) and
// gallery (3x3 → ≈150x130) tiles fall below it and keep the small stand-in.
// The height floor (300) is well above any multi-tile cell yet below the
// wind_steer dial region, so only genuinely large rects get the dial.
static inline bool windrose_is_fullscreen(int w, int h) {
    return w >= 400 && h >= 300;
}

// Full-screen detection for the autopilot HUD: a single autopilot leaf over a
// nudge-button row (col-split [4,1]) lands ≈ 480x384 for the leaf — the same
// large-rect threshold as the wind dial. Below this (gallery 3x3 ≈ 150x130) the
// element keeps its small state-pill + nudge-row stand-in.
static inline bool autopilot_is_fullscreen(int w, int h) {
    return w >= 400 && h >= 300;
}

// Full-screen detection for the wind-steering composite: a single windsteer leaf
// over a nudge-button row (col-split [3,1]) lands a near-full-width region — same
// large-rect threshold as the wind dial / autopilot HUD. Below this (gallery 3x3
// ≈ 150x130) the element falls back to the small windrose stand-in.
static inline bool windsteer_is_fullscreen(int w, int h) {
    return w >= 400 && h >= 300;
}

// Small wind-rose stand-in: a dashed warn ring with the AWS hero number and
// apparent/true wind chevrons. Shared by the multi-tile windrose tile AND the
// non-fullscreen windsteer fallback (a windsteer leaf below the dial threshold
// renders this same compact rose). Drives via the WindRose marker-update branch.
static void paint_wind_rose_standin(TileState &t, const MetricBinding &m, int w, int h) {
    // Round-fit (#50): value-only when the fitted circle cannot hold the ring;
    // else the ring is sized to the circle and the wind heads ride 6 px
    // further in so their tips stay inside it (round_safe::dial_overhang).
    if (t.fit_dia < 0) {
        paint_value_only_body(t, m, w, h, "88.8");
        return;
    }
    const int mk_in = t.fit_dia > 0 ? 6 : 0;
    int dia = (w < h ? w : h) - 56;
    if (dia < 80) dia = 80;
    if (t.fit_dia > 0) dia = t.fit_dia;
    lv_obj_t *ring = lv_obj_create(t.root);
    lv_obj_set_size(ring, dia, dia);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    // style.color replaces the ring and hero (web dial.ts opts.color); the wind
    // heads, sectors and authored markers keep their own colours.
    const uint32_t rose_rgb = elem_rgb(m, theme.warn);
    lv_obj_set_style_border_color(ring, lv_color_hex(rose_rgb), 0);
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
    t.markers = ui::build_marker_ring(t.root, mcx, mcy, dia / 2 - mk_in, wind_markers, 2,
                                      /*occlude_lower=*/false);

    // style.sectors: bow-relative arcs just inside the warn ring (close-hauled
    // / no-go zones; this rose is bow-up so screen angle == authored angle).
    // Built before the hero so they stay behind it.
    build_sector_arcs(t.root, m, mcx, mcy, dia / 2 - 5, 6, LV_OPA_70, t.sector_arcs,
                      t.sector_last_s, t.sector_last_e, t.sector_last_hidden);

    // style.hull: boat silhouette at the rose centre (bow-up).
    if (m.hull) add_hull(ring, dia / 2, dia / 2, dia / 2 - 10);

    // Authored MIDL markers[]: rim at the ring radius, vector markers inside.
    t.markers_auth = build_authored_ring(t.root, m, mcx, mcy, dia / 2 - mk_in, (dia / 2) * 62 / 100,
                                         /*occlude_lower=*/false);

    // Wind speed (AWS) is the hero number, sized to dominate the ring (the
    // marker is now outside, so the number can fill the dial).
    // Fit inside the ring with air for its border and the orbiting wind heads
    // (the chord narrows away from the centre line, so stay well under `dia`).
    const lv_font_t *vfont = ring_hero_font("88.8", dia * 84 / 100);
    // Round-fit: the wind heads now orbit over the ring's inner edge.
    if (mk_in) vfont = compact_font("88.8", dia - 2 * 12, dia / 2);
    t.value = lv_label_create(ring);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(rose_rgb), 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, m.unit && m.unit[0] ? -8 : 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);

    // Unit ("kn") under the AWS hero number so the wind dial is consistent with
    // the SOG / DEPTH numeric tiles (was a bare number with no unit).
    if (m.unit && m.unit[0]) {
        t.unit = lv_label_create(ring);
        lv_label_set_text(t.unit, m.unit);
        lv_obj_set_style_text_font(t.unit, &font_b612_14, 0);
        lv_obj_set_style_text_color(t.unit, lv_color_hex(theme.fg_dim), 0);
        lv_obj_align_to(t.unit, t.value, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(t.unit, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void paint_wind_rose_body(TileState &t, const MetricBinding &m, int w, int h) {
    // Full-screen single-leaf windrose → the high-fidelity rotating dial. The
    // dial reads boat::View directly (no MIDL binding). t.aux holds the dial root,
    // whose user_data is the WindDialState the update path retrieves; the
    // last_aux_pct == -2 sentinel marks "full dial" (stand-in leaves it -1).
    if (windrose_is_fullscreen(w, h)) {
        WindDialState *st = winddial::build(t.root, w, h, elem_rgb(m, theme.warn));
        if (st) {
            t.aux = st->root;
            t.last_aux_pct = -2;
            return;
        }
        // Fall through to the stand-in if the PSRAM alloc failed.
    }
    paint_wind_rose_standin(t, m, w, h);
}

// Wind-steering composite: full-screen heading-up dial with laylines, else the
// compact wind-rose stand-in (reused from the windrose tile). The composite
// reads boat::View directly (no MIDL binding); t.aux holds its root, whose
// user_data is the WindSteerState the update path retrieves. last_aux_pct == -2
// marks "full composite" (stand-in leaves it -1, and is driven by the WindRose
// marker branch in update).
static void paint_wind_steer_body(TileState &t, const MetricBinding &m, int w, int h) {
    if (windsteer_is_fullscreen(w, h)) {
        WindSteerState *st = windsteer::build(t.root, w, h, elem_rgb(m, theme.good));
        if (st) {
            t.aux = st->root;
            t.last_aux_pct = -2;
            return;
        }
        // Fall through to the stand-in if the PSRAM alloc failed.
    }
    paint_wind_rose_standin(t, m, w, h);
}

// Text widget: monospace value centered. Mirrors editor .wpreview .text-val.
static void paint_text_body(TileState &t, const MetricBinding &m, int /*w*/, int h) {
    // Text widget covers multi-line values like position (lat / lon on
    // two lines). Pick a font that won't overflow short tiles.
    const lv_font_t *vfont = &font_b612_20;
    if (h >= 160) vfont = &font_b612_28;
    t.text_cap = vfont;  // fit_text_font steps down (then ellipsizes) from here
    t.value = lv_label_create(t.root);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, vfont, 0);
    lv_obj_set_style_text_color(t.value, lv_color_hex(value_color(m)), 0);
    lv_obj_set_style_text_align(t.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 6);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
}

// Button widget: rounded accent bubble. Mirrors editor .wpreview .btn-bubble.
static void paint_button_body(TileState &t, const MetricBinding &m, int w, int h) {
    // Fill MOST of the tile so the bubble is a large finger target (styled like
    // the autopilot nudge pills, scaled up). Leave a margin so the tile's panel
    // chrome edge still shows. Height shrinks a bit more on short tiles.
    int btn_w = w - 24;
    int btn_h = (h < 150) ? (h - 40) : (h - 56);
    if (btn_w < 32) btn_w = 32;
    if (btn_h < 24) btn_h = 24;

    lv_obj_t *btn = lv_obj_create(t.root);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(value_color(m)), 0);  // style.color or accent
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    // Pressed feedback (the tile root receives the click; this is purely visual).
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    // Not itself clickable: the action handler lives on the tile root (build_tile),
    // so the indev hit-test must walk past this bubble to the root. Without this,
    // the bubble would swallow the tap and the action would never fire.
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(btn);
    const char *ltxt = (m.label && m.label[0]) ? m.label : "TAP";
    lv_label_set_text(label, ltxt);
    // Scale the label by bubble HEIGHT first. Must be a Montserrat font —
    // font_xl_64 is digits-only and would drop letters from labels like "HOME".
    const lv_font_t *lfont = &font_b612_20;
    if (btn_h >= 60) lfont = &font_b612_28;
    if (btn_h < 36) lfont = &font_b612_14;
    // Then fit by WIDTH: on a narrow tile (e.g. the wind_steer 6-button row at
    // ~80 px) a height-picked 28/20 px font would clip a 4-letter label like
    // "TACK"/"GYBE". Step the font DOWN the 28→20→14 ladder until the measured
    // label width fits the bubble (minus a small horizontal pad). Roomy buttons
    // keep their large height-picked font (the first ladder entry already fits).
    {
        static const lv_font_t *const ladder[] = {&font_b612_28, &font_b612_20, &font_b612_14};
        const int max_w = btn_w - 12;  // small horizontal pad inside the bubble
        // Find where the height-picked font sits in the ladder; never step UP
        // from it (width-fit only shrinks).
        int start = 1;  // default font_b612_20
        if (lfont == &font_b612_28)
            start = 0;
        else if (lfont == &font_b612_14)
            start = 2;
        for (int i = start; i < 3; ++i) {
            lv_point_t sz;
            lv_text_get_size(&sz, ltxt, ladder[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lfont = ladder[i];
            if (sz.x <= max_w) break;  // fits — stop shrinking
        }
    }
    lv_obj_set_style_text_font(label, lfont, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x001218), 0);  // dark ink
    lv_obj_center(label);
    t.aux = btn;
}

// Style the stand-in autopilot state pill for engaged/standby. All colors are
// derived from the live theme tokens — engaged is a `good` tint over the panel
// with a `good` border/label, standby a hollow panel_edge outline with dim
// text — so red-night shows an amber-toned engaged state, classic a deep
// green on cream, etc. (the old hardcoded 0x143b2a forest fill leaked the
// original dark palette into every theme). Shared by the painter (initial
// standby look) and both update paths (quad-grid + freeform) on state flips.
// `m`'s style.color replaces the engaged colour (web autopilotSvg); standby stays dim.
static void style_ap_pill(lv_obj_t *pill, lv_obj_t *value, bool engaged, const MetricBinding &m) {
    if (!pill || !value) return;
    if (engaged) {
        const uint32_t on = elem_rgb(m, theme.good);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(mix_rgb(on, theme.panel, 30)), 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(on), 0);
        lv_obj_set_style_text_color(value, lv_color_hex(on), 0);
    } else {
        lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(theme.panel_edge), 0);
        lv_obj_set_style_text_color(value, lv_color_hex(theme.fg_dim), 0);
    }
}

// Autopilot widget: state pill (filled `good` tint when engaged, hollow when
// standby) + target text + 4 nudge buttons row. Mirrors editor .wpreview .ap.
static void paint_autopilot_body(TileState &t, const MetricBinding &m, int w, int h) {
    // Full-screen autopilot leaf → the high-fidelity HUD (semicircular heading-up
    // compass, HDG/COG/CTS/AP-target marker ring, XTE strip, numeric tiles). The
    // HUD reads boat::View directly (no MIDL binding). t.aux holds the HUD root,
    // whose user_data is the ApHudState the update path retrieves; the
    // last_aux_pct == -2 sentinel marks "full HUD" (stand-in leaves it -1). Nudge
    // buttons are authored as sibling button leaves, NOT drawn here.
    if (autopilot_is_fullscreen(w, h)) {
        ApHudState *st = aphud::build(t.root, w, h, elem_rgb(m, theme.good));
        if (st) {
            t.aux = st->root;
            t.last_aux_pct = -2;
            return;
        }
        // Fall through to the stand-in if the PSRAM alloc failed.
    }

    // State pill. Built in the standby (hollow) look; the update paths flip it
    // to the engaged tint via style_ap_pill when the live apState says so.
    lv_obj_t *pill = lv_obj_create(t.root);
    lv_obj_set_size(pill, w - 32, 26);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_radius(pill, 4, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    t.aux2 = pill;

    t.value = lv_label_create(pill);
    lv_label_set_text(t.value, "--");
    lv_obj_set_style_text_font(t.value, &font_b612_20, 0);
    lv_obj_align(t.value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(t.value, LV_OBJ_FLAG_CLICKABLE);
    style_ap_pill(pill, t.value, false, m);

    // Target text below the pill.
    t.secondary = lv_label_create(t.root);
    lv_label_set_text(t.secondary, "target ---");
    lv_obj_set_style_text_font(t.secondary, &font_b612_14, 0);
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
        lv_obj_set_style_bg_color(b, lv_color_hex(theme.button_bg), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, labels[i]);
        lv_obj_set_style_text_font(bl, &font_b612_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(theme.button_fg), 0);
        lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    }
}

// Round panels (#50): MIDL display classes round-* keep tile content inside the
// visible circle (midl round-safe.ts). The panel background still fills the cell.
#if defined(YB_MIDL_CLASS_ROUND_360) || defined(YB_MIDL_CLASS_ROUND_240)
static constexpr bool kRoundPanel = true;
#else
static constexpr bool kRoundPanel = false;
#endif

// Transparent stand-in for the tile panel at a round-safe rect: same border +
// pad metrics as style_panel, so the painters lay out exactly as they would in
// a panel of that size. `cx`/`cy` are cell-local outer coordinates.
static lv_obj_t *make_content_box(lv_obj_t *panel, int cx, int cy, int w, int h) {
    const int inset = chrome::panel_border + chrome::panel_pad;
    lv_obj_t *o = lv_obj_create(panel);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, cx - inset, cy - inset);  // children sit in the panel's content box
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, chrome::panel_border, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, chrome::panel_pad, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

// `disc` (round panels only, else null): the visible circle in screen coords.
static TileState build_tile(lv_obj_t *parent, int x, int y, int w, int h, const MetricBinding &m,
                            const ui::round_safe::Disc *disc = nullptr) {
    TileState t = {};
    t.idx = -1;
    t.kind = m.kind;
    t.last_aux_pct = -1;
    t.last_zone_rgb = 0xFFFFFFFF;  // unset: a first zone colour of 0x000000 must apply
    strncpy(t.last_value, "\xFF", sizeof(t.last_value));
    strncpy(t.last_secondary, "\xFF", sizeof(t.last_secondary));
    for (int i = 0; i < 4; ++i)
        strncpy(t.last_extras[i], "\xFF", sizeof(t.last_extras[0]));

    // A full-screen windrose / autopilot leaf renders a high-fidelity composite
    // that draws its own rim/face/labels and fills the whole rect — the
    // glass-cockpit panel border + top-left cap would frame it and clip the
    // rotating bezel/scale. Suppress the chrome: a borderless, padless,
    // background-only root with no caption, so the composite fills the tile and
    // its local (0..w / 0..h) coordinates line up with the rect.
    bool fullscreen_dial = ((m.kind == WidgetKind::WindRose) && windrose_is_fullscreen(w, h)) ||
                           ((m.kind == WidgetKind::Autopilot) && autopilot_is_fullscreen(w, h)) ||
                           ((m.kind == WidgetKind::WindSteer) && windsteer_is_fullscreen(w, h));

    // --- Chrome: glass-cockpit panel (gradient + border via style_panel) ---
    // No left accent rail (dropped per design feedback); semantic color lives
    // in the value text instead. style_panel applies the consolidated tokens.
    t.root = lv_obj_create(parent);
    lv_obj_set_size(t.root, w, h);
    lv_obj_set_pos(t.root, x, y);
    if (fullscreen_dial) {
        lv_obj_set_style_bg_color(t.root, lv_color_hex(theme.bg), 0);
        lv_obj_set_style_bg_opa(t.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(t.root, 0, 0);
        lv_obj_set_style_radius(t.root, 0, 0);
        lv_obj_set_style_pad_all(t.root, 0, 0);
        lv_obj_clear_flag(t.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t.root, LV_OBJ_FLAG_EVENT_BUBBLE);
    } else {
        style_panel(t.root);
    }

    // Chrome caption (top-left dim label). Buttons render their own label inside
    // the accent bubble (paint_button_body), so a chrome cap would duplicate it —
    // skip it for Button tiles. The full-screen dial draws its own labels, so skip
    // the cap there too. t.cap stays null; update paths never touch it.
    // Round panel (#50): caption and content move into the part of the cell
    // inside the visible circle (round-safe rect; dials into the fitted circle).
    // The full-screen composites draw their own round-aware faces and keep the
    // whole cell. Painters lay out in `body` (a transparent stand-in panel at the
    // safe rect) and see its size as (w, h); t.root stays the cell panel so the
    // click / zoom handlers and update paths are unchanged.
    const bool has_cap = m.kind != WidgetKind::Button && !fullscreen_dial;
    int cap_x = 10, cap_y = 2;  // caption position, cell content-box coords
    bool small_cap = false;
    lv_obj_t *body = nullptr;
    if (disc && !fullscreen_dial) {
        // Ring-shaped painters fit a circle; the arc gauge is one too on the
        // device (an lv_arc ring, unlike the web's flat gauge). The compass
        // band (style.shape "band") is not a ring: it lays out in the safe rect.
        using ui::round_safe::Dial;
        Dial dial = Dial::None;
        if (m.kind == WidgetKind::Compass && m.shape != DialShape::Band) dial = Dial::Compass;
        if (m.kind == WidgetKind::WindRose) dial = Dial::WindRose;
        if (m.kind == WidgetKind::Gauge) dial = Dial::Gauge;
        const size_t cap_len = (has_cap && m.label) ? strlen(m.label) : 0;
        const ui::round_safe::TilePlan plan =
            ui::round_safe::plan_tile({x, y, w, h}, *disc, dial, cap_len);
        cap_x += plan.cap.x - x;
        cap_y += plan.cap.y - y;
        // Ring painters get a box that centres a ring of plan.ring_dia on the
        // fitted circle (the box itself is larger; only the ring is drawn). A
        // circle too small for the painter's ring falls back to value-only
        // content in the safe rect, so nothing spills into a neighbouring cell.
        const ui::round_safe::IRect &b = plan.body;
        if (plan.ring_dia > 0) t.fit_dia = plan.ring_dia;
        if (plan.value_only) t.fit_dia = -1;
        // A plain numeric tile needs ~110 px of height to clear its caption.
        if (m.kind == WidgetKind::Numeric && m.extras_count == 0 && !plan.hidden && b.h < 110)
            t.fit_dia = -1;
        // A narrow safe rect gets the small caption font so the caption is
        // not cut off at the cell edge.
        small_cap = plan.cap.w < 110;
        body = make_content_box(t.root, b.x - x, b.y - y, b.w, b.h);
        // A cell (almost) entirely under the bezel keeps its panel and nothing
        // else; the painters still build (hidden) so the update paths have
        // their widgets.
        if (plan.hidden) lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
        w = b.w;
        h = b.h;
        t.body_w = w;
    }

    if (has_cap) {
        t.cap = lv_label_create(t.root);
        lv_label_set_text(t.cap, m.label ? m.label : "");
        lv_obj_set_style_text_font(t.cap, small_cap ? &font_b612_14 : &font_b612_20, 0);
        lv_obj_set_style_text_color(t.cap, lv_color_hex(theme.fg_dim), 0);
        lv_obj_set_pos(t.cap, cap_x, cap_y);
        lv_obj_clear_flag(t.cap, LV_OBJ_FLAG_CLICKABLE);
        if (body && lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(t.cap, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Body: dispatched per widget kind ---
    lv_obj_t *const panel_root = t.root;
    if (body) t.root = body;
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
    case WidgetKind::WindSteer:
        paint_wind_steer_body(t, m, w, h);
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
    case WidgetKind::Clinometer:
        paint_clinometer_body(t, m, w, h);
        break;
    case WidgetKind::Numeric:
    default:
        paint_numeric_body(t, m, w, h);
        break;
    }
    t.root = panel_root;

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

// ---------------------------------------------------------------------------
// Freeform builder — one tile per solver-provided pixel rect.
//
// build_tile() dispatches to the per-kind painters (numeric, compass, gauge,
// bar, wind_rose, wind_steer, text, button, autopilot, trend, clinometer).
// Called directly by midl_render_apply. Tile state is heap-allocated (PSRAM)
// and attached via lv_obj_set_user_data.

// Maximum freeform tiles per screen — mirrors the MIDL solver bound so a
// PlacementSet can be mapped 1:1. Spec-derived; see include/midl_limits.h.
static constexpr int FREEFORM_MAX_TILES = (int)midl::FirmwareLimits::max_tiles_per_screen;

// Per-tile state extends TileState with the tile's own pixel width so the
// update path can fit fonts to the solver rect.
struct FreeformTile {
    TileState tile;  // reuse the full per-tile widget + cache state
    int tile_w;      // pixel width of this tile (from the solver rect)
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

    // Round display classes (#50): content stays inside the visible circle.
    ui::round_safe::Disc disc = ui::round_safe::viewport_disc({0, 0, (double)LCD_W, (double)LCD_H});
    const ui::round_safe::Disc *round_disc = kRoundPanel ? &disc : nullptr;

    for (int i = 0; i < n; ++i) {
        const Rect &r = rects[i];
        st->tiles[i].tile = build_tile(root, r.x, r.y, r.w, r.h, spec.metrics[i], round_disc);
        st->tiles[i].tile.idx = i;
        st->tiles[i].tile_w = st->tiles[i].tile.body_w ? st->tiles[i].tile.body_w : r.w;

        // Tap-to-zoom: a zoomable element, or any live metric by default.
        const MetricBinding &mb = spec.metrics[i];
        bool interactive =
            mb.zoom_target ? (layout::zoom_action(mb.zoomable, mb.zoom_target) != layout::ZOOM_NONE)
                           : (mb.source != MetricSource::None || (mb.path && mb.path[0]));
        if (interactive) {
            lv_obj_add_flag(st->tiles[i].tile.root, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(st->tiles[i].tile.root, tile_zoom_action_cb, LV_EVENT_CLICKED,
                                (void *)&spec.metrics[i]);
        }
    }

    lv_obj_set_user_data(root, st);
    // Freed on teardown: MIDL screens are rebuilt live by apply_all (config
    // push / theme switch) — without this every rebuild leaked one
    // FreeformState per screen.
    lv_obj_add_event_cb(root, free_user_data_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(root, trend_root_deleted_cb, LV_EVENT_DELETE, nullptr);
    return root;
}

void update_freeform(lv_obj_t *root, const ScreenVariantSpec &spec, const boat::View &data) {
    if (!root) return;
    auto *st = (FreeformState *)lv_obj_get_user_data(root);
    if (!st) return;
    s_fmt = config::format();  // snapshot display formatting for this refresh
    // Trend history records only for the screen on display: (re)bind its
    // series when this root becomes the shown one. Every refresh merges at
    // most one finished backfill.
    if (s_trend_owner != root) {
        TileState *tiles[FREEFORM_MAX_TILES];
        for (int i = 0; i < st->count; ++i)
            tiles[i] = &st->tiles[i].tile;
        trend_activate(root, tiles, st->count, spec);
    }
    trend_poll_backfill();
    const double trend_now = trend_now_s();

    for (int i = 0; i < st->count; ++i) {
        TileState &t = st->tiles[i].tile;
        if (t.idx < 0 || t.idx >= spec.metric_count) continue;
        const MetricBinding &m = spec.metrics[t.idx];

        char pri[40], sec[24];
        format_metric(m, data, pri, sizeof(pri), sec, sizeof(sec));

        // Per-kind aux updates.
        switch (t.kind) {
        case WidgetKind::Gauge:
        case WidgetKind::Bar:
            // Fill / centre-zero needle, centre label, style.zones colour
            // tracking. See update_gauge_bar.
            update_gauge_bar(t, m, data);
            break;
        case WidgetKind::Autopilot:
            // Full-screen HUD: drives its own widgets from boat::View
            // (last_aux_pct == -2 sentinel set by paint_autopilot_body). This is
            // the freeform path that a single autopilot leaf hits.
            if (t.last_aux_pct == -2) {
                if (t.aux) aphud::update((ApHudState *)lv_obj_get_user_data(t.aux), data);
                break;
            }
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            // Engaged/standby pill restyle on state flips only:
            // last_aux_pct doubles as the styled-state
            // cache for the stand-in (-1 unset, 0 standby, 1 engaged).
            {
                bool engaged = data.apState[0] && strcmp(data.apState, "standby") != 0;
                int want = engaged ? 1 : 0;
                if (t.last_aux_pct != want) {
                    t.last_aux_pct = want;
                    style_ap_pill(t.aux2, t.value, engaged, m);
                }
            }
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
            // Band composite (style.shape "band"): drives its own rotating
            // scale + heading-relative markers/sectors (last_aux_pct == -2
            // sentinel set by paint_compass_band_body).
            if (t.last_aux_pct == -2) {
                update_compass_band(t, m, data, pri);
                break;
            }
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
            // HDG/CTS inner ring + COG outer ring: the
            // two-ring split keeps an HDG≈COG overlap from merging into one glyph.
            // An authored `bindings.dir` retargets the diamond pointer at the dir
            // source instead of CTS.
            {
                MetricBinding hdg = {}, cog = {}, cts = {};
                hdg.source = MetricSource::HDG_deg;
                cog.source = MetricSource::COG_deg;
                cts.source = MetricSource::CTS_deg;
                double dia_deg =
                    binding_has_dir(m) ? binding_dir_deg(m, data) : metric_scalar(cts, data);
                ui::MarkerSpec inner[2] = {
                    {metric_scalar(hdg, data), ui::Glyph::Triangle, true, theme.accent},
                    {dia_deg, ui::Glyph::Diamond, true, theme.alarm},
                };
                ui::marker_ring_update(t.markers, inner, 2, /*reference=*/0.0);
                ui::MarkerSpec outer[1] = {
                    {metric_scalar(cog, data), ui::Glyph::Triangle, false, theme.good},
                };
                ui::marker_ring_update(t.markers_cog, outer, 1, /*reference=*/0.0);
            }
            // Authored markers/sectors (north-up round dial: reference 0).
            update_authored_ring(t.markers_auth, m, data, 0.0);
            update_sector_arcs(m, data, 0.0, t.sector_arcs, t.sector_last_s, t.sector_last_e,
                               t.sector_last_hidden);
            break;
        case WidgetKind::Trend:
            // Hero number is the live reading; the chart is the time-windowed
            // history of the same value (update_trend).
            ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri);
            update_trend(t, m, data, trend_now);
            break;
        case WidgetKind::Clinometer:
            update_clinometer(t, m, data);
            break;
        default:
            // Full-screen wind dial / steering composite: drive their own widgets
            // from boat::View (last_aux_pct == -2 sentinel). This is the freeform
            // path that the single-leaf windrose / col-split windsteer screen hits.
            if (t.kind == WidgetKind::WindRose && t.last_aux_pct == -2) {
                if (t.aux) winddial::update((WindDialState *)lv_obj_get_user_data(t.aux), data);
                break;
            }
            if (t.kind == WidgetKind::WindSteer && t.last_aux_pct == -2) {
                if (t.aux) windsteer::update((WindSteerState *)lv_obj_get_user_data(t.aux), data);
                break;
            }
            // Numeric, WindRose, Text — value/secondary text slots.
            if (ui::set_text_if_changed(t.value, t.last_value, sizeof(t.last_value), pri)) {
                if (t.kind == WidgetKind::Numeric && t.hero_h > 0) {
                    // Fullscreen hero: re-fit the transform_scale to the new value
                    // width (1-digit scales bigger than 3-digit; see fit_hero_scale).
                    fit_hero_scale(t.value, pri, t.hero_h, t.hero_max_w);
                } else if (t.kind == WidgetKind::Numeric && t.fit_dia < 0) {
                    // Round-fit compact value (#50): fit its own budget.
                    lv_obj_set_style_text_font(t.value, compact_font(pri, t.fit_w, t.fit_h), 0);
                } else if (t.kind == WidgetKind::Numeric && m.extras_count == 0 &&
                           m.size_role == 0) {
                    // Fit the tile's own width (the solver rect) minus the
                    // caption/unit margin.
                    // An authored style.size role pins the font instead.
                    fit_value_font(t.value, pri, st->tiles[i].tile_w - 56);
                } else if (t.kind == WidgetKind::Text) {
                    // Step the font down to the tile's real width, else
                    // ellipsize (sys.version / sys.build on the status tiles).
                    fit_text_font(t.value, pri, st->tiles[i].tile_w - 24, t.text_cap);
                }
            }
            if (t.value &&
                (m.source == MetricSource::VMG_kn || m.source == MetricSource::VMGwind_kn)) {
                double vmg =
                    mps_to_kn(m.source == MetricSource::VMGwind_kn ? data.vmgWind : data.vmg);
                uint32_t col = isnan(vmg) ? theme.fg : (vmg < 0 ? theme.alarm : theme.good);
                lv_obj_set_style_text_color(t.value, lv_color_hex(col), 0);
            }
            if (t.secondary) {
                ui::set_text_if_changed(t.secondary, t.last_secondary, sizeof(t.last_secondary),
                                        sec);
            }
            if (t.kind == WidgetKind::WindRose || t.kind == WidgetKind::WindSteer) {
                MetricBinding awa = {}, twa = {};
                awa.source = MetricSource::AWA_deg;
                twa.source = MetricSource::TWA_deg;
                // An authored `bindings.dir` drives the warn pointer instead of
                // the built-in AWA head.
                double warn_deg =
                    binding_has_dir(m) ? binding_dir_deg(m, data) : metric_scalar(awa, data);
                ui::MarkerSpec wind[2] = {
                    {warn_deg, ui::Glyph::ChevronIn, true, theme.warn},
                    {metric_scalar(twa, data), ui::Glyph::ChevronOut, false, theme.good},
                };
                ui::marker_ring_update(t.markers, wind, 2, /*reference=*/0.0);
                // Authored markers/sectors (bow-up rose: reference 0).
                update_authored_ring(t.markers_auth, m, data, 0.0);
                update_sector_arcs(m, data, 0.0, t.sector_arcs, t.sector_last_s, t.sector_last_e,
                                   t.sector_last_hidden);
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

}  // namespace ui::layouts
