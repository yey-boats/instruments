#pragma once

// Single source of truth for physical-unit conversions.
//
// Historically the knots/degrees/Kelvin/nautical-mile factors were
// open-coded inline at every ingest and display site (0.514444, M_PI/180,
// +273.15, *1852.0). That scattered the constants across the source
// adapters (source_nmea_wifi.cpp, source_nmea2000.cpp) and the UI, so a
// precision or correctness fix had to be made in N places. Everything that
// converts between SI and human/marine units routes through here.
//
// Deliberately dependency-free (only <math.h>) so host-only TUs (parsers,
// Unity tests, the sim harness) can link it without pulling Arduino.

#include <math.h>

namespace units {

// Exact marine constants.
constexpr double kKnotMps = 1852.0 / 3600.0;  // 1 kn = 0.5144444... m/s
constexpr double kNmM = 1852.0;               // 1 nautical mile in metres
constexpr double kDegRad = M_PI / 180.0;      // degrees -> radians
constexpr double kKelvinOffset = 273.15;      // Celsius -> Kelvin

// Speed.
constexpr double kn_to_mps(double kn) {
    return kn * kKnotMps;
}
constexpr double mps_to_kn(double mps) {
    return mps / kKnotMps;
}

// Distance.
constexpr double nm_to_m(double nm) {
    return nm * kNmM;
}
constexpr double m_to_nm(double m) {
    return m / kNmM;
}

// Angle.
constexpr double deg_to_rad(double deg) {
    return deg * kDegRad;
}
constexpr double rad_to_deg(double rad) {
    return rad / kDegRad;
}

// Temperature.
constexpr double c_to_k(double c) {
    return c + kKelvinOffset;
}
constexpr double k_to_c(double k) {
    return k - kKelvinOffset;
}

// Normalise a heading/angle in radians to (-pi, pi]. Equivalent to the
// single `if (a > M_PI) a -= 2*M_PI` guards that NMEA2000 wind PGNs used
// for inputs already in [0, 2pi), but safe for any input.
inline double wrap_pi(double r) {
    while (r > M_PI)
        r -= 2.0 * M_PI;
    while (r < -M_PI)
        r += 2.0 * M_PI;
    return r;
}

// Normalise to [0, 2pi).
inline double wrap_2pi(double r) {
    while (r < 0.0)
        r += 2.0 * M_PI;
    while (r >= 2.0 * M_PI)
        r -= 2.0 * M_PI;
    return r;
}

}  // namespace units
