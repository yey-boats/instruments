#pragma once

// Host-clean POD types shared between ui_layouts.h (device) and
// midl_render.h (host-testable mapper). Extracted from ui_layouts.h so
// that translation units that only need the enum/struct bridge — notably
// midl_render.cpp and its Unity tests — do not pull in <lvgl.h>.
//
// ui_layouts.h #includes this file, so existing device code is unaffected.
// New host-only TUs include ONLY this header (not ui_layouts.h).

#include <stdint.h>

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
    Rudder_deg,  // rad_to_deg of d.rudder, signed with port/stbd helm suffix
    Position,    // formatted to current pos_format
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
    // resolved target string: "auto"/"" -> scale in place on the zoom screen,
    // any "<screenId>" -> switch to that screen. Both trail `kind` so existing
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

// Pixel-space rectangle used by the freeform builder to place one tile
// at the solver's output coordinates. Field order matches midl::Placement.rect
// so the caller can memcpy / brace-init without a conversion helper.
struct Rect {
    int x, y, w, h;
};

}  // namespace ui::layouts
