#pragma once

// Shared UI helpers used by all screen modules.

#include <Arduino.h>
#include <math.h>
#include <stddef.h>

#include "units.h"

namespace ui {

// Position display format (matches the global g_pos_format in main.cpp).
enum class PosFormat : uint8_t { DDM = 0, DD = 1, DMS = 2 };

// Format a lat/lon pair into a two-line string.
void format_position(double lat, double lon, PosFormat fmt, char *buf, size_t cap);

// Current position format (NVS-backed via main.cpp).
PosFormat pos_format();
void set_pos_format(PosFormat f);
const char *pos_format_name(PosFormat f);

uint8_t brightness();
void set_brightness(int value);
double depth_alarm_m();
void set_depth_alarm_m(double value);
double battery_alarm_v();
void set_battery_alarm_v(double value);

// Spec 17 §8 v1 commands. The alarm banner is reused as the overlay
// surface - overlay_show paints a transient message; overlay_clear
// removes it. Both must be called on the LVGL task (drained via
// app::pump). The plugin / manager posts ShowOverlay / ClearOverlay
// app::Commands; never call these directly from net / BLE / manager
// worker tasks.
void overlay_show(const char *message);
void overlay_clear();

// Unit helpers. The raw conversion factors live in units.h (single source
// of truth); these thin wrappers add the display-specific normalisation.
inline double rad_to_deg_pos(double rad) {
    double d = units::rad_to_deg(rad);
    if (d < 0) d += 360;
    return d;
}
inline double mps_to_kn(double mps) {
    return units::mps_to_kn(mps);
}
inline double k_to_c(double k) {
    return units::k_to_c(k);
}

// Trim radians to [-180,180] degrees (relative wind angles).
inline double rad_to_deg_pm(double rad) {
    double d = units::rad_to_deg(rad);
    while (d > 180)
        d -= 360;
    while (d < -180)
        d += 360;
    return d;
}

}  // namespace ui
