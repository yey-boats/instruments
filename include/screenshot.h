#pragma once

// Cross-task screenshot for the web UI. lv_snapshot_take walks the LVGL
// widget tree and must run on the LVGL task. The web task posts a
// request, blocks on a semaphore; the LVGL refresh loop fulfils it and
// returns a heap-allocated BMP buffer the caller is responsible for
// freeing.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace screenshot {

// Initialise the semaphore. Safe to call from setup().
void setup();

// Called from the LVGL task each frame. Cheap when no request is pending.
void serve_pending();

// Called from any other task (e.g. the web task). Blocks up to
// timeout_ms; returns true on success, with *out_bmp and *out_len set.
// Caller must heap_caps_free(*out_bmp) when done.
bool request(uint32_t timeout_ms, uint8_t **out_bmp, size_t *out_len);

}  // namespace screenshot
