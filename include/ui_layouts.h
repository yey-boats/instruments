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
    WaterTemp_C,
    BatteryV,
    BatterySOC_pct,
    DTW,  // formatted as nm or m
    BTW_deg,
    XTE,
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

}  // namespace ui::layouts
