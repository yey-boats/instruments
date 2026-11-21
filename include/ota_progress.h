// Pure rules behind the on-device firmware-update screen (instruments#106).
//
// While a firmware OTA writes flash, the RGB panel's PSRAM scan-out loses
// timing on the Arduino 2.x build (no bounce buffers), so whatever is on the
// panel drifts and duplicates sideways. The theory: shifted copies of a
// near-uniform black frame are barely visible. So before the first flash
// write the panel is switched to a mostly black "Updating firmware" screen
// with a thin progress bar, and the backlight stays lit (Mode::Lit).
// Mode::Dark (compile with -D YEYBOATS_OTA_SCREEN_DARK=1) is the comparison
// fallback: backlight off for the whole write, as #107 does for record applies.
//
// The bar is redrawn only at whole `step_pct` steps (or every
// `indeterminate_step` bytes when the image size is unknown), and each redraw
// happens between Update.write calls, never during one.
//
// No allocation, no platform deps: host-tested in test/test_ota_progress. The
// device half (LVGL screen + UI-task handshake) is src/ota_screen.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef YEYBOATS_OTA_SCREEN_DARK
#define YEYBOATS_OTA_SCREEN_DARK 0
#endif

namespace ota_progress {

enum class Mode : uint8_t { Lit, Dark };

constexpr Mode kMode = YEYBOATS_OTA_SCREEN_DARK ? Mode::Dark : Mode::Lit;

inline const char *mode_name(Mode m) {
    return m == Mode::Dark ? "dark" : "lit";
}

// Whole percent written, 0..100; -1 when the total is unknown.
inline int percent(uint32_t done, uint32_t total) {
    if (total == 0) return -1;
    if (done >= total) return 100;
    return (int)(((uint64_t)done * 100u) / total);
}

// Decides when the bar needs a redraw. One instance per OTA.
class Pacer {
  public:
    explicit Pacer(uint8_t step_pct = 4, uint32_t indeterminate_step = 128u * 1024u)
        : step_(step_pct ? step_pct : 1), ind_step_(indeterminate_step ? indeterminate_step : 1) {}

    // True when (done, total) moved the bar to a new step. The first call
    // after reset() is always due; 100 % is always due exactly once.
    bool due(uint32_t done, uint32_t total) {
        if (total) {
            int p = percent(done, total);
            int q = p >= 100 ? 100 : (p / step_) * step_;
            if (q == last_) return false;
            last_ = q;
            return true;
        }
        uint32_t bucket = done / ind_step_;
        if (bucket == bucket_) return false;
        bucket_ = bucket;
        last_ = -1;
        return true;
    }

    // Bar value at the last due() (step-quantized percent), -1 = unknown.
    int shown_pct() const { return last_; }
    // Indeterminate animation phase: advances once per due() without a total.
    uint32_t phase() const { return bucket_ == UINT32_MAX ? 0 : bucket_; }

    void reset() {
        last_ = -2;
        bucket_ = UINT32_MAX;
    }

  private:
    uint8_t step_;
    uint32_t ind_step_;
    int last_ = -2;
    uint32_t bucket_ = UINT32_MAX;
};

// Indeterminate bar: a segment of `seg` px that bounces across `width` px.
// Returns the segment's left edge for animation `phase`.
inline int bounce_x(uint32_t phase, int width, int seg, int stride = 0) {
    int span = width - seg;
    if (span <= 0) return 0;
    if (stride <= 0) stride = span / 8 > 0 ? span / 8 : 1;
    int steps = (span + stride - 1) / stride;  // one sweep, end to end
    int period = steps * 2;
    int k = (int)(phase % (uint32_t)period);
    int pos = (k <= steps ? k : period - k) * stride;
    return pos > span ? span : pos;
}

// "1.2 / 2.0 MB", or "1.2 MB" when the total is unknown.
inline void format_bytes(char *out, size_t n, uint32_t done, uint32_t total) {
    if (!out || !n) return;
    unsigned d10 = (unsigned)(((uint64_t)done * 10u + 524288u) / 1048576u);
    if (total) {
        unsigned t10 = (unsigned)(((uint64_t)total * 10u + 524288u) / 1048576u);
        snprintf(out, n, "%u.%u / %u.%u MB", d10 / 10, d10 % 10, t10 / 10, t10 % 10);
    } else {
        snprintf(out, n, "%u.%u MB", d10 / 10, d10 % 10);
    }
}

}  // namespace ota_progress
