#pragma once

// Touch screen calibration: 6-parameter affine transform.
//
//   sx = a*rx + b*ry + c
//   sy = d*rx + e*ry + f
//
// Computed from 4-point correspondences (raw GT911 coordinates -> screen
// coordinates) using least-squares. The default values are identity
// (a=1, b=0, c=0, d=0, e=1, f=0) so an uncalibrated device behaves as
// before. Values persist in NVS namespace "touch_cal".

#include <Arduino.h>
#include <stdint.h>

namespace touch_cal {

struct Matrix {
    float a, b, c;  // x coefficients
    float d, e, f;  // y coefficients
};

struct Sample {
    int16_t raw_x, raw_y;
    int16_t target_x, target_y;
};

// Load calibration from NVS (or identity if none stored). Idempotent.
void setup();

// Apply current calibration to a raw GT911 sample, in place.
void apply(int16_t *x, int16_t *y);

// Replace the active matrix and persist to NVS.
void set(const Matrix &m);

// Read the active matrix.
Matrix current();

// Compute a best-fit matrix from N >= 3 sample correspondences and
// return whether the solve succeeded. Does NOT persist - call set()
// after to commit.
bool solve(const Sample *samples, size_t n, Matrix &out);

// Whether the active matrix is the default identity (i.e. the user has
// never calibrated).
bool is_default();

// Restore identity in RAM and clear NVS keys - effective immediately,
// no reboot needed. Useful as a recovery path when a bad calibration
// has shifted the panel coordinates so far that LVGL hit-testing fails.
void reset();

}  // namespace touch_cal
