#pragma once

// Pure (no-LVGL) marker-ring math: where a marker sits on a dial and whether a
// semicircular dial occludes it, plus the glyph token<->enum map the editor and
// firmware share. Host-compiled and unit-tested under [env:native]; the LVGL
// rendering lives in ui_markers.{h,cpp} and calls into this.

#include <math.h>
#include <stdint.h>

namespace ui {

// Keep in lockstep with ui::Glyph in ui_markers.h and the manifest "glyphs"
// list in src/capabilities.cpp. marker_math owns the canonical order so the
// token<->index map is testable without LVGL.
enum class GlyphId : uint8_t {
    Triangle = 0,
    Diamond,
    Circle,
    Bar,
    Cross,
    ChevronIn,
    ChevronOut,
    ChevronLeft,
    ChevronRight,
    ChevronDouble,
    COUNT
};

constexpr uint8_t kMaxMarkersPerDial = 12;

// Screen angle of a marker: value - reference, normalized to [0,360).
// 0 = top of the dial (under the lubber). NaN in -> NaN out (caller hides it).
inline double marker_screen_angle(double value_deg, double reference_deg) {
    if (isnan(value_deg) || isnan(reference_deg)) return NAN;
    double a = value_deg - reference_deg;
    a = fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

// True if a semicircular (top-half) dial occludes this screen angle. The top
// half spans [-half_window, +half_window] around 0; anything further round is
// in the hidden lower half. Matches the degree-label hide at |rel| > 96 in
// ui_compass.cpp. NaN -> occluded (hidden).
inline bool marker_occluded(double screen_angle_deg, double half_window_deg) {
    if (isnan(screen_angle_deg)) return true;
    double rel = screen_angle_deg;
    if (rel > 180.0) rel -= 360.0;  // -> [-180,180]
    return rel > half_window_deg || rel < -half_window_deg;
}

// Glyph token (manifest/editor string) -> GlyphId. Returns COUNT on unknown.
GlyphId glyph_from_token(const char *token);

// GlyphId -> stable token string. Returns "" for COUNT/out-of-range.
const char *glyph_to_token(GlyphId g);

}  // namespace ui
