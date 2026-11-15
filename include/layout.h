#pragma once

// Shared fixed bounds + the per-field zoom decision used by the MIDL render
// path (midl_limits.h, midl_render.cpp, ui_layouts.cpp's freeform painter).
// The legacy layout::Config pipeline that used to live here is gone: the
// display renders MIDL records only (docs/midl/midl-only.md).
// Pure C++; compiles on host for unit tests as well as the device.

#include <stddef.h>

namespace layout {

constexpr size_t MAX_SCREENS = 8;  // screens per MIDL doc (midl::FirmwareLimits)
constexpr size_t STR_LEN = 32;     // ids, names, units, zoom targets (incl NUL)
constexpr size_t PATH_LEN = 96;    // SignalK paths / local ids (incl NUL)

// ---- Per-field zoom decision (pure; host-tested) -------------------------

// What a tap on a (possibly zoomable) field should do.
enum ZoomAction {
    ZOOM_NONE = 0,     // field is not zoomable; tap does nothing
    ZOOM_AUTO_SCALE,   // scale the field up in place on the zoom screen
    ZOOM_SHOW_SCREEN,  // switch to the referenced full screen
};

// Resolve a tap action from a field's `zoomable` flag and `zoom` target
// string. Not zoomable → NONE. Zoomable with an empty or "auto" target →
// AUTO_SCALE. Zoomable with any other (non-empty) target → SHOW_SCREEN (the
// caller validates the screen id exists before navigating).
ZoomAction zoom_action(bool zoomable, const char *zoom);

}  // namespace layout
