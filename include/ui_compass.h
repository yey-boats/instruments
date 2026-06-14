#pragma once

#include <lvgl.h>
#include <stdint.h>

// Reference glass-cockpit primitives shared by the autopilot HUD (and any
// future heading view). A semicircular compass band, rounded numeric tiles, and
// a horizontal cross-track-error strip. All colors/metrics come from
// ui::theme / ui::chrome (no inline magic) so day/night stay in lockstep.

namespace ui {

// Semicircular heading compass. The white band + green rail + lubber are fixed;
// the `scale` ring (degree numbers + ticks) and `bug` (target marker) are
// rotated by the refresh path via ui::set_rot_if_changed():
//   scale -> -heading          (current heading rides under the top lubber)
//   bug   -> (target - heading) (amber bug sits on the rail at the target)
// (cx, cy, r) are in the returned root's local coordinates; the root clips to
// the top semicircle so the lower half of the rotating ring never shows.
struct Compass {
    lv_obj_t *root;
    lv_obj_t *scale;
    lv_obj_t *bug;
    int cx, cy, r;
};

// Build a compass occupying a `w`-wide region at (ox, oy) in `parent`.
Compass build_compass(lv_obj_t *parent, int ox, int oy, int w);

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
