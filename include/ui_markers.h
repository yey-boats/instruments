#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "marker_math.h"  // ui::GlyphId, kMaxMarkersPerDial, placement math

// Shared marker-ring primitive for every compass-like widget (the semicircular
// autopilot HUD, the round Compass tile, the wind rose / wind-steer dials). One
// glyph set, one placement contract (screen_angle = value - reference). All
// colors are passed in from ui::theme by the caller (no inline magic here).

namespace ui {

using Glyph = GlyphId;  // the firmware enum is the pure-math enum

// One marker's presentation + current bearing. value_deg is refreshed each
// frame from the screen's data snapshot; NaN hides the marker.
struct MarkerSpec {
    double value_deg;  // current bearing (degrees, 0=N); NaN -> hidden
    Glyph glyph;
    bool filled;
    uint32_t color;  // resolved theme token
};

// An orbiting ring of up to kMaxMarkersPerDial markers, concentric with a dial
// of radius `r` centered at local (cx,cy) in `parent`. Each marker is a holder
// pivoting at the dial center; rotating it sweeps the glyph around the rim
// pointing inward. occlude_lower hides markers in the bottom half (semicircle).
struct MarkerRing {
    lv_obj_t *holder[kMaxMarkersPerDial];
    int16_t last_rot[kMaxMarkersPerDial];
    int8_t last_hidden[kMaxMarkersPerDial];
    uint8_t count;
    bool occlude_lower;
    int r;
};

// Build a 28x28 glyph visual (filled or outline) on a PSRAM canvas. Exposed for
// reuse (e.g. a legend); the ring uses it internally.
lv_obj_t *draw_glyph(lv_obj_t *parent, Glyph g, bool filled, uint32_t color);

// Build the ring. specs[].glyph/filled/color are fixed at build; specs[].value_deg
// is read later by marker_ring_update. count is clamped to kMaxMarkersPerDial.
MarkerRing build_marker_ring(lv_obj_t *parent, int cx, int cy, int r, const MarkerSpec *specs,
                             uint8_t count, bool occlude_lower);

// Place each marker at marker_screen_angle(specs[i].value_deg, reference_deg);
// hide NaN/occluded. Cheap: only changed rotations/visibility touch LVGL.
void marker_ring_update(MarkerRing &ring, const MarkerSpec *specs, uint8_t count,
                        double reference_deg);

}  // namespace ui
