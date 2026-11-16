// Blanked transition for MIDL record applies (instruments#106, short-term fix
// until the spec-21 bounce-buffer env). See include/apply_blank_machine.h for
// the rules. Safe from any task: web/manager handlers call begin/end (via
// Guard) and expect_render; the UI task calls rendered(), frame_flushed() and
// poll(). The backlight is driven raw through board::set_backlight; the
// user's configured brightness (config::ui().brightness) is never changed and
// is what restore() puts back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdint.h>

namespace apply_blank {

// A flash write is about to start: cut the backlight if lit. Returns the
// window epoch to pass to end().
uint32_t begin();
void end(uint32_t epoch);
// A MidlActivate was queued: keep the window open until it renders.
void expect_render();
// UI task: an activation finished; `changed` = the screen was rebuilt.
void rendered(bool changed);
// UI task (flush callback): the last area of a frame was flushed.
void frame_flushed();
// UI task, after lv_timer_handler: restores the backlight when the window
// is over (or a safety timeout fired) and logs the dark duration.
void poll();

// RAII write window. Construct armed, or arm() lazily right before the first
// flash write so a refused request (400/409/413/422) never blanks.
class Guard {
  public:
    explicit Guard(bool armed = false) {
        if (armed) arm();
    }
    ~Guard() {
        if (armed_) end(epoch_);
    }
    void arm() {
        if (armed_) return;
        epoch_ = begin();
        armed_ = true;
    }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

  private:
    bool armed_ = false;
    uint32_t epoch_ = 0;
};

}  // namespace apply_blank
