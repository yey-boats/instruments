#pragma once

// Layout templates per docs/specs/11-layout-templates-screen-variants.md.
//
// A screen becomes a variant of a small template catalog:
//
//   screen = template + metric bindings + tap commands
//
// This file declares the template ids, metric binding shape, and the
// factory entry points. Each template lives in ui_layouts.cpp (or
// later, in dedicated layout_<name>.cpp files).
//
// Phase 1 lands the framework + the `quad_grid` template (2x2 tile
// dashboard). Other templates are stubbed and registered as they're
// implemented; un-implemented ones return NULL so callers can decide
// whether to fall back to a hand-built screen.

#include <lvgl.h>
#include <stdint.h>
#include "signalk_parser.h"

namespace sk {
class SubscriptionSet;
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

// Identifies which sk::Data field feeds a metric. The factory keeps
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
    Position,  // formatted to current pos_format
    APState,
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
};

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
    // resolved target string: "auto"/"" → scale in place on the zoom screen,
    // any "<screenId>" → switch to that screen. Both trail `kind` so existing
    // positional brace-init tables value-init them (zoomable=false, zoom=NULL).
    bool zoomable;
    const char *zoom_target;
};

struct ScreenVariantSpec {
    const char *screen_id;
    const char *title;
    TemplateId template_id;
    const MetricBinding *metrics;
    uint8_t metric_count;
    uint8_t variant_flags;  // template-specific; e.g. quad_grid: 0 = footer, 1 = no footer
};

// Build the screen widget tree for `spec`. Returns NULL if the
// template isn't implemented yet. Caller passes the returned root to
// ui::register_screen.
lv_obj_t *create(lv_obj_t *parent, const ScreenVariantSpec &spec);

// Update displayed values from a snapshot. Idempotent and uses the
// dirty-value caches in include/ui_dirty.h so the partial-render path
// stays cold when nothing changed.
void update(lv_obj_t *root, const ScreenVariantSpec &spec, const sk::Data &data);

// Drive the full-screen "zoom" view to a chosen metric (normally set by a tile
// tap; also used by the sim harness and remote controllers).
void set_zoom_target(const MetricBinding &m);

// Reverse of layout_renderer's path_to_source(): the canonical SignalK path a
// MetricSource consumes, or nullptr for non-path sources (None). Position and
// APState ARE real paths a screen subscribes (navigation.position /
// steering.autopilot.state) and so map to their strings. Used by the per-screen
// subscription manager (collect_paths) and the perf-counter store shadow.
const char *source_to_path(MetricSource s);

// Collect every SignalK path the screen `spec` consumes (primary sources plus
// every extras[] row) into `out`. `out` is NOT cleared first (so callers can
// accumulate the baseline + a screen). Non-path sources are skipped. Pure /
// host-testable; takes no LVGL or live state.
void collect_paths(const ScreenVariantSpec &spec, sk::SubscriptionSet &out);

}  // namespace ui::layouts
