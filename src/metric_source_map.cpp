// metric_source_map.cpp — pure string-to-enum bridges, host-testable.
//
// Extracted from layout_renderer.cpp so that host-only TUs (midl_render,
// Unity tests) can link against path_to_source / widget_to_kind without
// pulling in the full device-only layout_renderer.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "layout_renderer.h"

#include <string.h>

namespace ui::layout_render {

using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

// NOTE: This map is SEPARATE from midl::render::token_to_kind (midl_render.cpp).
// It handles legacy editor camelCase tokens ("numeric","windRose"); token_to_kind
// handles MIDL lowercase tokens ("single-value","windrose"). Do not merge these two maps.
WidgetKind widget_to_kind(const char *widget) {
    if (!widget || !widget[0]) return WidgetKind::Numeric;
    if (strcmp(widget, "compass") == 0) return WidgetKind::Compass;
    if (strcmp(widget, "gauge") == 0) return WidgetKind::Gauge;
    if (strcmp(widget, "bar") == 0) return WidgetKind::Bar;
    if (strcmp(widget, "windRose") == 0) return WidgetKind::WindRose;
    if (strcmp(widget, "windSteer") == 0) return WidgetKind::WindSteer;
    if (strcmp(widget, "autopilot") == 0) return WidgetKind::Autopilot;
    if (strcmp(widget, "text") == 0) return WidgetKind::Text;
    if (strcmp(widget, "button") == 0) return WidgetKind::Button;
    if (strcmp(widget, "trend") == 0) return WidgetKind::Trend;
    return WidgetKind::Numeric;
}

MetricSource path_to_source(const char *p) {
    if (!p || !p[0]) return MetricSource::None;
    if (strcmp(p, "environment.wind.speedApparent") == 0) return MetricSource::AWS_kn;
    if (strcmp(p, "environment.wind.angleApparent") == 0) return MetricSource::AWA_deg;
    if (strcmp(p, "environment.wind.speedTrue") == 0) return MetricSource::TWS_kn;
    if (strcmp(p, "environment.wind.angleTrueWater") == 0) return MetricSource::TWA_deg;
    if (strcmp(p, "navigation.speedOverGround") == 0) return MetricSource::SOG_kn;
    if (strcmp(p, "navigation.speedThroughWater") == 0) return MetricSource::STW_kn;
    if (strcmp(p, "navigation.courseOverGroundTrue") == 0) return MetricSource::COG_deg;
    if (strcmp(p, "navigation.headingTrue") == 0) return MetricSource::HDG_deg;
    if (strcmp(p, "navigation.position") == 0) return MetricSource::Position;
    if (strcmp(p, "environment.depth.belowTransducer") == 0) return MetricSource::Depth_m;
    if (strcmp(p, "environment.depth.belowKeel") == 0) return MetricSource::DepthKeel_m;
    if (strcmp(p, "environment.water.temperature") == 0) return MetricSource::WaterTemp_C;
    if (strcmp(p, "electrical.batteries.house.voltage") == 0) return MetricSource::BatteryV;
    if (strcmp(p, "electrical.batteries.house.stateOfCharge") == 0)
        return MetricSource::BatterySOC_pct;
    if (strcmp(p, "navigation.courseRhumbline.nextPoint.distance") == 0) return MetricSource::DTW;
    if (strcmp(p, "navigation.courseRhumbline.nextPoint.bearingTrue") == 0)
        return MetricSource::BTW_deg;
    if (strcmp(p, "navigation.courseRhumbline.crossTrackError") == 0) return MetricSource::XTE;
    if (strcmp(p, "navigation.courseRhumbline.velocityMadeGood") == 0) return MetricSource::VMG_kn;
    if (strcmp(p, "navigation.courseRhumbline.bearingTrackTrue") == 0) return MetricSource::CTS_deg;
    if (strcmp(p, "steering.rudderAngle") == 0) return MetricSource::Rudder_deg;
    if (strcmp(p, "steering.autopilot.state") == 0) return MetricSource::APState;
    return MetricSource::None;
}

}  // namespace ui::layout_render
