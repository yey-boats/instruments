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
    if (rel > 180.0) rel -= 360.0;  // -> [-180,180]; 180 stays 180 (> any sane half_window)
    return rel > half_window_deg || rel < -half_window_deg;
}

// Radial stagger levels for a cluster of dial markers. A marker within
// `threshold_deg` of an EARLIER visible marker steps one level further inward
// so near-coincident bearings stay individually identifiable instead of
// stacking into one blob at the rim. NaN angles (hidden markers) always get
// level 0 and never push the others. Levels cap at 3 (four stacked glyphs is
// the practical dial maximum). Pure/host-tested; the pixel step per level
// lives with the renderer (ui_markers.cpp).
inline void marker_stagger_levels(const double *screen_angle_deg, uint8_t count,
                                  double threshold_deg, uint8_t *level_out) {
    for (uint8_t i = 0; i < count; ++i) {
        level_out[i] = 0;
        if (isnan(screen_angle_deg[i])) continue;
        for (uint8_t j = 0; j < i; ++j) {
            if (isnan(screen_angle_deg[j])) continue;
            double da = fabs(screen_angle_deg[i] - screen_angle_deg[j]);
            if (da > 180.0) da = 360.0 - da;
            if (da <= threshold_deg && (uint8_t)(level_out[j] + 1) > level_out[i])
                level_out[i] = (uint8_t)(level_out[j] + 1);
        }
        if (level_out[i] > 3) level_out[i] = 3;
    }
}

// Center-zero strip deflection for a signed cross-track error, as a fraction
// of full scale in [-1, +1]. NEGATIVE = deflect toward the PORT (left) end.
// The firmware renders XTE text as magnitude + side letter with positive
// xte -> 'P' (ui::format_xte and the QuadGrid XTE tile share that convention),
// so the needle must deflect toward the side the letter names: positive xte
// -> 'P' -> port -> negative fraction. Out-of-range clamps to the end stop;
// NaN -> 0 (callers leave the needle centered when there is no fix).
inline double xte_needle_frac(double xte_m, double full_scale_m) {
    if (isnan(xte_m) || !(full_scale_m > 0)) return 0.0;
    double f = -xte_m / full_scale_m;
    if (f > 1.0) f = 1.0;
    if (f < -1.0) f = -1.0;
    return f;
}

// Glyph token (manifest/editor string) -> GlyphId. Returns COUNT on unknown.
GlyphId glyph_from_token(const char *token);

// GlyphId -> stable token string. Returns "" for COUNT/out-of-range.
const char *glyph_to_token(GlyphId g);

}  // namespace ui
