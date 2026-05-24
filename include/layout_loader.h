#pragma once

// Layout loader: owns the live `Config` and is responsible for getting
// it from somewhere (right now: baked-in default; next PR: SignalK
// REST + NVS cache).

#include <Arduino.h>
#include "layout.h"

namespace layout {

// Initialise the loader. On first call, parses the baked-in default
// layout into the live config. Returns true on success.
bool load_default();

// Read-only access to the currently active config.
const Config &current();

// Print a one-screen-per-line summary via net::logf. Used by console
// `layout-show`.
void show_summary();

// Handle layout-related console lines. Returns true if consumed.
bool handleSerialCommand(const String &line);

}  // namespace layout
