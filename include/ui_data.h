#pragma once

// Shared UI helpers used by all screen modules.

#include <Arduino.h>
#include <math.h>
#include <stddef.h>

namespace ui {

// Position display format (matches the global g_pos_format in main.cpp).
enum class PosFormat : uint8_t { DDM = 0, DD = 1, DMS = 2 };

// Format a lat/lon pair into a two-line string.
void format_position(double lat, double lon, PosFormat fmt, char *buf, size_t cap);

// Current position format (NVS-backed via main.cpp).
PosFormat pos_format();
void set_pos_format(PosFormat f);
const char *pos_format_name(PosFormat f);

// Unit helpers
inline double rad_to_deg_pos(double rad) {
    double d = rad * 180.0 / M_PI;
    if (d < 0) d += 360;
    return d;
}
inline double mps_to_kn(double mps) { return mps * 1.94384; }
inline double k_to_c(double k) { return k - 273.15; }

// Trim radians to [-180,180] degrees (relative wind angles).
inline double rad_to_deg_pm(double rad) {
    double d = rad * 180.0 / M_PI;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

}  // namespace ui
