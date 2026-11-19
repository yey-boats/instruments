// Device half of the blanked record-apply transition (instruments#106). The
// pure rules live in include/apply_blank_machine.h (host-tested).
//
// One FreeRTOS mutex serializes the machine AND the backlight write, so a
// web-task cut and a UI-task restore can never land out of order (a restore
// decided before a new cut cannot re-light the panel mid-write). The mutex is
// a leaf lock: callers may hold the midl store lock; nothing here takes it.
// Logging happens after the mutex is released.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "apply_blank.h"

#if defined(YEYBOATS_HARNESS) || defined(APPLY_BLANK_OFF)

// Headless harness: no panel, no UI task to restore from. APPLY_BLANK_OFF (the #106
// bounce-buffer spike env): keep the backlight on during record applies, so any drift
// left after the real fix is visible instead of hidden by the blanking.
namespace apply_blank {
uint32_t begin() {
    return 0;
}
void end(uint32_t) {
}
void expect_render() {
}
void rendered(bool) {
}
void frame_flushed() {
}
void poll() {
}
}  // namespace apply_blank

#else

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "apply_blank_machine.h"
#include "board.h"
#include "config_runtime.h"
#include "net.h"

namespace apply_blank {
namespace {

StaticSemaphore_t s_mtx_buf;
SemaphoreHandle_t s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
Machine s_m;                     // ~40 bytes, internal RAM is fine
volatile uint32_t s_frames = 0;  // completed frames (UI task writes)

struct Lock {
    Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mtx); }
};

}  // namespace

uint32_t begin() {
    uint32_t epoch = 0;
    bool cut = false;
    {
        Lock l;
        cut = s_m.begin(millis(), epoch) == Action::Cut;
        if (cut) board::set_backlight(0);  // raw: config brightness untouched
    }
    if (cut) net::logf("[apply] backlight dark (flash write)");
    return epoch;
}

void end(uint32_t epoch) {
    Lock l;
    s_m.end(millis(), epoch);
}

void expect_render() {
    Lock l;
    s_m.expect_render(millis());
}

void rendered(bool changed) {
    Lock l;
    s_m.rendered(millis(), changed, s_frames);
}

void frame_flushed() {
    s_frames = s_frames + 1;
}

void poll() {
    uint32_t ms = 0;
    Reason why = Reason::None;
    {
        Lock l;
        if (!s_m.dark()) return;
        if (s_m.poll(millis(), s_frames) != Action::Restore) return;
        // The user's configured value (config_runtime), read-only: the
        // temporary 0 was never persisted. config's lock is a leaf too.
        board::set_backlight(::config::ui().brightness);
        ms = s_m.last_dark_ms();
        why = s_m.last_reason();
    }
    net::logf("[apply] backlight dark %u ms%s%s", (unsigned)ms, why == Reason::Done ? "" : " -- ",
              why == Reason::Done ? "" : reason_name(why));
}

}  // namespace apply_blank

#endif  // YEYBOATS_HARNESS
