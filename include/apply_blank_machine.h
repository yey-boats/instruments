// Pure state machine behind the "blanked transition" (instruments#106).
//
// While a MIDL record write is in flight (LittleFS write/erase) the RGB panel's
// PSRAM scan-out loses timing on the Arduino 2.x Arduino_ESP32RGBPanel build
// (no bounce buffers), so the panel shows drifted/duplicated content. Until the
// IDF-5 bounce-buffer env lands (spec 21), the backlight is cut for the window:
//
//   first write begins  -> Cut (backlight 0)
//   writes end, queued activations render, one full frame is flushed, and
//   `linger_ms` passes without a new event -> Restore (user's brightness)
//
// Safety: `idle_timeout_ms` after the last event, or `max_dark_ms` after the
// cut, poll() restores whatever is still outstanding (a failed apply, a lost
// activation, a stuck guard). Each dark window has an epoch; a guard that ends
// after a timeout reset belongs to an old epoch and is ignored, so it cannot
// release a newer window early.
//
// No allocation, no platform deps: host-tested in test/test_apply_blank. The
// device wrapper (src/apply_blank.cpp) serializes calls with a mutex and does
// the backlight writes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdint.h>

namespace apply_blank {

enum class Action : uint8_t { None, Cut, Restore };
enum class Reason : uint8_t { None, Done, IdleTimeout, MaxDark };

struct Timing {
    uint32_t linger_ms = 150;         // quiet time before restoring (bridges a multi-PUT push)
    uint32_t idle_timeout_ms = 3000;  // no event for this long -> restore regardless
    uint32_t max_dark_ms = 10000;     // hard cap on one dark window
};

class Machine {
  public:
    explicit Machine(Timing t = Timing()) : t_(t) {}

    // A flash write that can disturb scan-out starts. Returns Cut when the
    // backlight was lit (first writer of a window). `epoch_out` identifies the
    // window for end().
    Action begin(uint32_t now, uint32_t &epoch_out) {
        Action a = Action::None;
        if (!dark_) {
            dark_ = true;
            since_ = now;
            writers_ = 0;
            renders_ = 0;
            need_frame_ = false;
            ++epoch_;
            a = Action::Cut;
        }
        if (writers_ < 255) ++writers_;
        last_ = now;
        epoch_out = epoch_;
        return a;
    }

    // The write started by begin() in window `epoch` finished.
    void end(uint32_t now, uint32_t epoch) {
        if (!dark_ || epoch != epoch_) return;  // window already restored/reset
        if (writers_) --writers_;
        last_ = now;
    }

    // An activation (MidlActivate) was queued for the UI task. Counted only
    // inside a dark window; a lit-screen activation does not blank.
    void expect_render(uint32_t now) {
        if (!dark_) return;
        if (renders_ < 255) ++renders_;
        last_ = now;
    }

    // UI task: a record activation finished (success or not). `changed` =
    // the screen was rebuilt, so wait for its first flushed frame (the
    // frame counter at this moment is `frames`).
    void rendered(uint32_t now, bool changed, uint32_t frames) {
        if (!dark_) return;
        if (renders_) --renders_;
        if (changed) {
            need_frame_ = true;
            frames_at_render_ = frames;
        }
        last_ = now;
    }

    // UI task, after each lv_timer_handler pass. `frames` counts completed
    // full flushes. Returns Restore once the window is over.
    Action poll(uint32_t now, uint32_t frames) {
        if (!dark_) return Action::None;
        Reason r = Reason::None;
        if (now - since_ >= t_.max_dark_ms)
            r = Reason::MaxDark;
        else if (now - last_ >= t_.idle_timeout_ms)
            r = Reason::IdleTimeout;
        else if (writers_ == 0 && renders_ == 0 && (!need_frame_ || frames != frames_at_render_) &&
                 now - last_ >= t_.linger_ms)
            r = Reason::Done;
        if (r == Reason::None) return Action::None;
        dark_ = false;
        writers_ = 0;
        renders_ = 0;
        need_frame_ = false;
        last_reason_ = r;
        last_dark_ms_ = now - since_;
        return Action::Restore;
    }

    bool dark() const { return dark_; }
    uint8_t writers() const { return writers_; }
    uint8_t renders() const { return renders_; }
    // Duration and reason of the last restored window.
    uint32_t last_dark_ms() const { return last_dark_ms_; }
    Reason last_reason() const { return last_reason_; }

  private:
    Timing t_;
    bool dark_ = false;
    bool need_frame_ = false;
    uint8_t writers_ = 0;
    uint8_t renders_ = 0;
    uint32_t epoch_ = 0;
    uint32_t since_ = 0;
    uint32_t last_ = 0;
    uint32_t frames_at_render_ = 0;
    uint32_t last_dark_ms_ = 0;
    Reason last_reason_ = Reason::None;
};

inline const char *reason_name(Reason r) {
    switch (r) {
    case Reason::Done:
        return "done";
    case Reason::IdleTimeout:
        return "idle timeout";
    case Reason::MaxDark:
        return "max-dark timeout";
    default:
        return "none";
    }
}

}  // namespace apply_blank
