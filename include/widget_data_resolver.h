#pragma once

// Spec 19 §"Data Path Resolution" - maps a widget's `path` string to
// a numeric or string value from sk::Data. Resolution order:
//
//   1. local normalized alias (boat.sog, boat.headingTrue, ...)
//   2. known SignalK parser field (navigation.speedOverGround, ...)
//   3. raw SignalK dynamic value (not implemented; returns NaN)
//   4. missing -> NaN
//
// Pure C++ - host-testable. Lives in its own header so D5 (render
// plan -> LVGL) can pull just the resolver without dragging in
// LVGL itself.

#include "signalk_parser.h"
#include <math.h>

namespace widget_data {

// Numeric value from a path. Returns NaN if unresolved/missing.
// Units are spec-19-natural for each metric:
//   boat.sog / .stw           m/s
//   boat.aws / .tws           m/s
//   boat.cogTrue/.headingTrue rad (0..2pi)
//   boat.awa / .twa           rad (signed, -pi..pi)
//   boat.depth                m
//   boat.batteryVoltage       V
//   boat.batterySoc           0..1
//   boat.autopilotTarget      rad
double resolve_numeric(const char *path, const sk::Data &d);

// String value for paths that yield enumerated states. Currently
// only `boat.autopilotState` / `steering.autopilot.state` produce
// non-empty strings; numeric paths return "" (caller should use
// resolve_numeric).
// `out` is null-terminated even when "missing"; returns true iff a
// non-empty value was written.
bool resolve_string(const char *path, const sk::Data &d, char *out, size_t cap);

// True iff the path is one of the recognised forms (either local
// alias or known SK field). Useful for early-validation in the
// config parser.
bool is_known(const char *path);

}  // namespace widget_data
