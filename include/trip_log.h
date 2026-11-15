#pragma once

// Trip log: locally-integrated odometer + stats (SignalK only provides the
// instantaneous SOG). Used to live inside the C++ trip screen and only
// integrated while that screen refreshed; it is now a screen-independent
// module ticked from the UI refresh timer, read by the MIDL `trip` record
// through local ids (trip.distance / trip.avgSpeed / trip.maxSpeed /
// trip.underway). Persisted to NVS ("trip").
//
// The accumulator (Accum + sample) is pure and host-tested; setup/tick/reset
// are device-side (NVS + boat data).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <math.h>
#include <stdint.h>

namespace trip_log {

constexpr double UNDERWAY_THRESHOLD_MPS = 0.5;  // ~1 kn

struct Accum {
    double dist_m;        // accumulated distance, metres
    uint32_t underway_s;  // accumulated seconds with sog > threshold
    double max_sog;       // m/s
    uint32_t last_ms;     // last integration sample (0 = none yet)
};

// Integrate one SOG sample (m/s, NaN = no data) at time now_ms. Samples are
// taken at most once a second. Returns true when the caller should persist
// (every 30 underway seconds).
bool sample(Accum &a, double sog_mps, uint32_t now_ms);

// Display values. distance in nm; avg/max in kn (avg is NaN until 5 s
// underway).
double distance_nm(const Accum &a);
double avg_kn(const Accum &a);
double max_kn(const Accum &a);

// ---- device ----------------------------------------------------------------
void setup();            // load from NVS
void tick();             // sample boat SOG; call from the UI refresh timer
void reset();            // zero + persist
const Accum &current();  // live accumulator (UI task)
uint32_t started_at_ms();

}  // namespace trip_log
