// Trip log accumulator, pure half. See include/trip_log.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "trip_log.h"

namespace trip_log {

bool sample(Accum &a, double sog, uint32_t now) {
    if (a.last_ms == 0) a.last_ms = now;
    if (isnan(sog) || now - a.last_ms < 1000) return false;
    double dt_s = (now - a.last_ms) / 1000.0;
    a.last_ms = now;
    bool underway = sog > UNDERWAY_THRESHOLD_MPS;
    if (underway) {
        a.dist_m += sog * dt_s;
        a.underway_s += (uint32_t)dt_s;
    }
    if (sog > a.max_sog) a.max_sog = sog;
    return underway && (a.underway_s % 30) == 0;
}

static constexpr double MPS_TO_KN = 3600.0 / 1852.0;

double distance_nm(const Accum &a) {
    return a.dist_m / 1852.0;
}

double avg_kn(const Accum &a) {
    if (a.underway_s <= 5) return NAN;
    return a.dist_m / a.underway_s * MPS_TO_KN;
}

double max_kn(const Accum &a) {
    return a.max_sog * MPS_TO_KN;
}

}  // namespace trip_log
