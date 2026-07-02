#pragma once

// Host-clean POD types shared between ui_layouts.h (device) and
// midl_render.h (host-testable mapper). Extracted from ui_layouts.h so
// that translation units that only need the enum/struct bridge — notably
// midl_render.cpp and its Unity tests — do not pull in <lvgl.h>.
//
// ui_layouts.h #includes this file, so existing device code is unaffected.
// New host-only TUs include ONLY this header (not ui_layouts.h).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace boat {
struct View;  // fwd — full definition in boat_data.h (not needed here)
}

namespace ui::layouts {

enum class TemplateId : uint8_t {
    QuadGrid = 0,
    HeroPlus,
    RoundInstrument,
    TrendChart,
    SplitPair,
    StatusList,
    ControlConsole,
    RouteProgress,
    SetupForm,
    AlertFocus,
    COUNT
};

// Identifies which boat::View field feeds a metric. The factory keeps
// the lookup pure - a controller layer can later replace this with
// arbitrary SignalK path strings.
enum class MetricSource : uint8_t {
    None = 0,
    AWS_kn,   // mps_to_kn(d.aws)
    AWA_deg,  // rad_to_deg_pos(d.awa) with port/stbd suffix
    TWS_kn,
    TWA_deg,
    SOG_kn,
    COG_deg,
    HDG_deg,
    Depth_m,
    DepthKeel_m,  // m below keel (d.depthKeel)
    WaterTemp_C,
    BatteryV,
    BatterySOC_pct,
    DTW,  // formatted as nm (always)
    BTW_deg,
    CTS_deg,  // course to steer, rad (navigation.courseRhumbline.bearingTrackTrue)
    XTE,      // signed cross-track error in m (P/S suffix)
    VMG_kn,
    Rudder_deg,  // rad_to_deg of d.rudder, signed with port/stbd helm suffix
    Position,    // formatted to current pos_format
    APState,
    STW_kn,      // mps_to_kn(d.stw) — appended (keep ordinals stable for any persisted configs)
    VMGwind_kn,  // mps_to_kn(d.vmgWind) — wind/polar VMG (performance.velocityMadeGood).
                 // VMG_kn stays = route/waypoint VMG (navigation.courseRhumbline.velocityMadeGood).
    // ---- coverage wave (environment / attitude / engine / power). APPEND-ONLY:
    // ordinals are load-bearing for persisted configs; new sources go last.
    OutsideTemp_C,        // k_to_c(d.outsideTemp)
    OutsidePressure_hPa,  // pa_to_hpa(d.outsidePressure), "1013" no decimals
    Humidity_pct,         // d.humidity * 100
    Roll_deg,             // signed deg, +ve = starboard heel (P/S suffix like rudder)
    Pitch_deg,            // signed deg, +ve = bow up
    ROT_degmin,           // radps_to_degmin(d.rateOfTurn), signed
    TripLog_nm,           // m_to_nm(d.tripLog)
    Log_nm,               // m_to_nm(d.totalLog)
    BattCurrent_A,        // d.battCurrent, signed A
    BattTemp_C,           // k_to_c(d.battTemp)
    EngineRpm,            // hz_to_rpm(d.engineRevs)
    EngineCoolant_C,      // k_to_c(d.engineCoolantTemp)
    EngineOilP_bar,       // pa_to_bar(d.engineOilPressure) — matches MIDL library
                          // engine-systems OIL format.unit "bar"
    EngineFuelRate_lph,   // m3s_to_lph(d.engineFuelRate)
    HDGm_deg,             // rad_to_deg_pos(d.headingMag)
    Variation_deg,        // signed deg, +E / -W
};

// Optional extra row beneath the primary value (multi-value tiles).
// Up to 4 extras per tile; rendered as "<label> <value>" small text.
struct MetricRow {
    const char *label;  // "COG", "HDG", "" for no prefix
    MetricSource source;
};

// Widget kind dispatched per-tile inside the QuadGrid template. Each
// kind has a dedicated painter in ui_layouts.cpp that mirrors the
// editor's widgetPreview() render so the device and layout editor
// agree visually. Default is Numeric for back-compat with the existing
// dashboard tile tables.
enum class WidgetKind : uint8_t {
    Numeric = 0,  // big accent value + unit + secondary line
    Compass,      // round bezel, heading number center, CTS marker
    Gauge,        // 270 degree arc, fill = primary fraction, percent center
    Bar,          // small title + horizontal bar showing primary (0..1)
    WindRose,     // dashed warn ring + center AWS value
    Autopilot,    // state pill + target + nudge buttons row
    Text,         // monospace value text (position, AP state strings)
    Button,       // rounded accent bubble label
    Trend,        // sparkline chart
    // APPEND-ONLY (0-based enum values are load-bearing for persisted configs and
    // the gnu++11 aggregate value-init of MetricBinding.kind). New kinds go last.
    WindSteer,   // semicircular heading-up steering dial with no-go + target laylines
    Clinometer,  // horizontal-arc bubble level: heel angle ±45° (port red /
                 // starboard green), pitch as small secondary readout.
                 // NOTE: MIDL token "clinometer" is a firmware EXTENSION —
                 // it is not in the midl catalog yet (upstream follow-up).
};

// Threshold colour band (MIDL style.zones): "value < lt -> this colour",
// evaluated low->high (see midl web model.ts resolveZoneColor / the schema).
// `lt` is in DISPLAY units. The colour is either a literal 0xRRGGBB or a theme
// token the painter resolves against the live ui::theme palette (so day/night
// flips keep working); Default means "painter fallback colour".
enum class ZoneColor : uint8_t {
    Default = 0,  // unknown token -> painter's per-kind fallback
    Literal,      // rgb field holds the authored #rrggbb
    Accent,
    Warn,
    Alarm,  // web tokens "alarm" / "bad" / "danger"
    Good,
    Port,
    Starboard,
};

struct MetricZone {
    float lt;         // upper bound (exclusive) of this band, display units
    uint32_t rgb;     // literal colour when color == ZoneColor::Literal
    ZoneColor color;  // token resolved by the painter against ui::theme
};

// Bound on authored zones carried per binding (good/warn/alarm + a sentinel
// top bucket is the idiomatic maximum).
constexpr uint8_t MAX_METRIC_ZONES = 4;

struct MetricBinding {
    const char *id;             // "wind", "depth", ... stable id for API addressing
    const char *label;          // "WIND" displayed caption
    const char *unit;           // "kn", "m", "deg" - shown next to primary value
    MetricSource source;        // primary value
    uint32_t accent;            // small color rail (0xRRGGBB)
    const char *target_screen;  // tap target (NULL = no nav action)
    // Optional secondary rows. extras_count <= 4. Leave at 0 for the
    // classic Hero/secondary tile (back-compat with the first quad_grid
    // demos that didn't carry these fields).
    uint8_t extras_count;
    MetricRow extras[4];
    // Last field so existing 6-arg brace-init MetricBinding tables get
    // value-initialized to 0 = WidgetKind::Numeric without us having to
    // add a default member initializer (which would make MetricBinding
    // non-aggregate under gnu++11, the Arduino-ESP32 default standard).
    WidgetKind kind;
    // Per-field zoom (Slice 6). `zoomable` gates the tap; `zoom_target` is the
    // resolved target string: "auto"/"" -> scale in place on the zoom screen,
    // any "<screenId>" -> switch to that screen. Both trail `kind` so existing
    // positional brace-init tables value-init them (zoomable=false, zoom=NULL).
    bool zoomable;
    const char *zoom_target;
    // Optional per-element value scaling/formatting from the MIDL `format` block.
    // ADDITIVE and trailing, with NO default member initializers — that would make
    // MetricBinding a non-aggregate under gnu++11 (the Arduino-ESP32 default std for
    // the base esp32-4848s040 env) and break every positional brace-init table that
    // stops at `kind`. They value-init to 0 instead. range_min==range_max==0 means
    // "painter uses its built-in default per-source range"; the painter only reads
    // `precision` inside the range_min!=range_max branch, so its value-init-to-0 on
    // legacy tables is never observed. The MIDL mapper sets precision=-1 explicitly
    // when format.precision is absent (see midl_render.cpp map_element).
    float range_min;   // gauge/bar lower bound (0 with range_max==0 -> default)
    float range_max;   // gauge/bar upper bound
    int8_t precision;  // value decimal places; -1 = painter default (set by mapper)
    // Button command target (MIDL action.kind == "command"). ADDITIVE trailing
    // field, no default member initializer (keeps MetricBinding an aggregate under
    // gnu++11 — see the range_* note above). NULL on every legacy/non-button tile.
    // nav-kind actions reuse `target_screen`; command-kind populates this and the
    // button_action_cb routes it through net::dispatchCommand.
    const char *command;
    // ---- MIDL presentation-parity fields (audit wave 2). ALL additive and
    // trailing with NO default member initializers (aggregate rule above);
    // legacy positional tables value-init them to 0/NULL/None. ----
    // Raw SignalK path retained when path_to_source() missed (source == None
    // but the dynamic PathStore can still serve it). Non-owning; points into
    // the caller's arena like id/label/unit. NULL on legacy bindings.
    const char *path;
    // Optional `bindings.dir` (dial pointer on compass/windrose can track a
    // different path than the centre value — midl types.ts:24, web dirDeg).
    MetricSource dir_source;  // enum bridge hit (None when absent/unknown)
    const char *dir_path;     // raw path retained on an enum miss (else NULL)
    // format.side: render a signed angle/offset as magnitude + P/S suffix
    // (web model.ts: positive -> 'S', negative -> 'P').
    bool side;
    // style.size role: 0 = auto (legacy height-based pick), 1..5 = S/M/L/XL/Fill
    // mapped onto the enabled font ladder {14,20,28,48,64}.
    uint8_t size_role;
    // style.center present: centre-zero deviation bar (XTE-style needle from
    // the middle) instead of a left-anchored fill.
    bool center_bar;
    // style.zones threshold bands (see MetricZone). zone_count <= MAX_METRIC_ZONES.
    uint8_t zone_count;
    MetricZone zones[MAX_METRIC_ZONES];
};

struct ScreenVariantSpec {
    const char *screen_id;
    const char *title;
    TemplateId template_id;
    const MetricBinding *metrics;
    uint8_t metric_count;
    uint8_t variant_flags;  // template-specific; e.g. quad_grid: 0 = footer, 1 = no footer
};

// Pixel-space rectangle used by the freeform builder to place one tile
// at the solver's output coordinates. Field order matches midl::Placement.rect
// so the caller can memcpy / brace-init without a conversion helper.
struct Rect {
    int x, y, w, h;
};

// ---------------------------------------------------------------------------
// Dynamic-path resolver injection. The painters (ui_layouts.cpp) resolve a
// MetricBinding.path through this function pointer; the MIDL render layer
// (midl_render_apply.cpp, device-only) installs a PathStore-backed resolver at
// apply time. Kept as an injection point so the host/sim builds of the
// painters never link the SignalK store. Returns the RAW (SI) value or NaN.
using DynamicResolveFn = double (*)(const char *path, const boat::View &d);
void set_dynamic_resolver(DynamicResolveFn fn);  // defined in ui_layouts.cpp

// ---------------------------------------------------------------------------
// Pure presentation helpers shared by the MIDL mapper, the painters, and the
// native unit tests (header-inline so no extra TU joins any build filter).

// True when `unit` classifies the value as an angle for format.side purposes.
// Mirrors web model.ts: unit "deg"/"rad" (or the ° literal) is an angle, and an
// ABSENT unit also counts as an angle (the web's `unit == null` branch).
inline bool unit_is_angle(const char *unit) {
    if (!unit || !unit[0]) return true;
    return strcmp(unit, "deg") == 0 || strcmp(unit, "rad") == 0 || strcmp(unit, "°") == 0;
}

// SI -> display-unit conversion for dynamic-path bindings (the enum sources
// carry their own conversion in metric_value). Mirrors the web renderer's
// FACTORS table (midl web format.ts); the source unit is assumed SI per the
// SignalK convention. Unknown/absent unit renders the raw value.
inline double convert_si_display(double v, const char *unit) {
    if (isnan(v) || !unit || !unit[0]) return v;
    if (strcmp(unit, "kn") == 0) return v * 1.943844492;  // m/s -> kn
    if (strcmp(unit, "km/h") == 0) return v * 3.6;        // m/s -> km/h
    if (strcmp(unit, "deg") == 0 || strcmp(unit, "°") == 0)
        return v * (180.0 / 3.14159265358979323846);  // rad -> deg
    if (strcmp(unit, "C") == 0 || strcmp(unit, "°C") == 0 || strcmp(unit, "degC") == 0)
        return v - 273.15;  // K -> C
    if (strcmp(unit, "F") == 0 || strcmp(unit, "°F") == 0 || strcmp(unit, "degF") == 0)
        return (v - 273.15) * 9.0 / 5.0 + 32.0;           // K -> F
    if (strcmp(unit, "%") == 0) return v * 100.0;         // ratio -> %
    if (strcmp(unit, "nm") == 0) return v / 1852.0;       // m -> nm
    if (strcmp(unit, "km") == 0) return v / 1000.0;       // m -> km
    if (strcmp(unit, "ft") == 0) return v * 3.280839895;  // m -> ft
    if (strcmp(unit, "h") == 0) return v / 3600.0;        // s -> h
    if (strcmp(unit, "min") == 0) return v / 60.0;        // s -> min
    if (strcmp(unit, "rpm") == 0) return v * 60.0;        // Hz -> rpm
    if (strcmp(unit, "bar") == 0) return v * 1e-5;        // Pa -> bar
    if (strcmp(unit, "kPa") == 0) return v * 1e-3;        // Pa -> kPa
    if (strcmp(unit, "hPa") == 0) return v * 1e-2;        // Pa -> hPa
    return v;
}

// First zone whose `lt` exceeds `v` (web model.ts resolveZoneColor semantics:
// "value below threshold -> this colour", evaluated low->high). -1 = no zone
// matched (v at/above every lt, or v is NaN) -> caller uses its fallback.
inline int zone_pick(const MetricZone *zones, uint8_t count, double v) {
    if (!zones || isnan(v)) return -1;
    for (uint8_t i = 0; i < count && i < MAX_METRIC_ZONES; ++i) {
        if (v < zones[i].lt) return (int)i;
    }
    return -1;
}

// format.side rendering: magnitude + P/S suffix. Positive -> 'S' (starboard),
// negative -> 'P' (port), matching the web renderer (model.ts / dial.ts). An
// angle value (display degrees) is first wrapped into [-180, 180] and shown
// with 0 decimals; a plain offset keeps its sign for the side and shows
// |v| at `precision` decimals (-1 -> 1 dp).
inline void format_side_value(double v, bool angle, int precision, char *out, size_t cap) {
    if (isnan(v)) {
        snprintf(out, cap, "--");
        return;
    }
    if (angle) {
        double s = fmod(v + 180.0, 360.0);
        if (s < 0) s += 360.0;
        s -= 180.0;  // [-180, 180]
        snprintf(out, cap, "%.0f%c", fabs(s), s >= 0 ? 'S' : 'P');
    } else {
        snprintf(out, cap, "%.*f%c", precision >= 0 ? precision : 1, fabs(v), v >= 0 ? 'S' : 'P');
    }
}

}  // namespace ui::layouts
