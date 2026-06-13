#pragma once

// Glue between the knob input events and the pure knob_menu state machine.
// Lives on the LVGL/UI task. apply_event() steps the machine, performs the
// resulting Action (autopilot PUT or remote view switch), and updates the
// menu overlay. Device-only.

#include "knob_menu.h"

namespace knob_ui {

void setup();                         // build overlay, init model + remote registry
void apply_event(int ev, bool held);  // ev = knob::Event; called from pump()

// Read-only access to the live menu model so the overlay can render the
// current level + highlight. Lives on the UI task; only the overlay reads it.
const knob::Model &model();

}  // namespace knob_ui
