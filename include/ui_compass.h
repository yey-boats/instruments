#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "ui_markers.h"

// Reference glass-cockpit primitives shared by the autopilot HUD (and any
// future heading view). A semicircular compass band, rounded numeric tiles, and
// a horizontal cross-track-error strip. All colors/metrics come from
// ui::theme / ui::chrome (no inline magic) so day/night stay in lockstep.

namespace ui {

// Inset (px) from the compass radius at which marker glyphs orbit on the
// semicircular HUD dials: MARGIN(18, ui_markers) + glyph half-extent lands them
// at ~r-26 on the white band, clear of the LABEL_INSET(44) degree labels.
constexpr int kSemiMarkerInset = 42;

// Semicircular heading compass. The white band + green rail + lubber are fixed;
// the `scale` ring (degree numbers + ticks) is rotated by -heading so the
// current heading rides under the top lubber (ui::set_rot_if_changed()).
// HDG/COG/CTS/target markers live in a separate ui::MarkerRing the screen builds
// on its OWN root (so glyphs that orbit just outside the rim are not clipped by
// this compass root, which is sized exactly to the dial) and drives via
// ui::marker_ring_update() with the heading as the reference.
// (cx, cy, r) are in the returned root's local coordinates; the root clips to
// the top semicircle so the lower half of the rotating ring never shows.
struct Compass {
    lv_obj_t *root;
    lv_obj_t *scale;         // rotating tick ring (rotated by -heading)
    lv_obj_t *nums[12];      // upright degree labels, repositioned per heading
    lv_obj_t *lubber;        // fixed red top lubber (heading reference)
    ui::MarkerRing markers;  // HDG/COG/CTS/target ring (built by the screen on its root)
    int cx, cy, r;
    int h;  // total root height (top semicircle + label clearance)
};

// Build a compass occupying a `w`-wide region at (ox, oy) in `parent`.
Compass build_compass(lv_obj_t *parent, int ox, int oy, int w);

// Reposition the upright degree labels so the heading sits at top. Call from the
// screen refresh whenever the heading changes (cheap; only moves 12 labels).
void compass_layout_labels(const Compass &cp, double hdg_deg);

// Rounded numeric tile (glass-cockpit card): caption top-left, unit top-right,
// big value centered below. Returns the value label for the refresh path.
lv_obj_t *numeric_tile(lv_obj_t *parent, int x, int y, int w, int h, const char *caption,
                       const char *unit, const lv_font_t *value_font, uint32_t value_color);

// Horizontal cross-track-error strip (PORT .. STBD, -1 .. +1). Translate the
// returned needle in refresh with lv_obj_set_x() within [cx - half, cx + half].
struct XteStrip {
    lv_obj_t *root;
    lv_obj_t *needle;
    int center_x;  // local x of the zero mark (within root)
    int half_px;   // pixels per full-scale (1.0 nm) deflection
};
XteStrip build_xte_strip(lv_obj_t *parent, int x, int y, int w, int h);

}  // namespace ui
