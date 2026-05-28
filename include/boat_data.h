#pragma once

// Source-neutral boat data model per docs/specs/12-nmea2000-and-visual-adoption.md.
//
// Multiple producers (SignalK, NMEA0183-over-WiFi, NMEA2000) publish into
// the same Snapshot. A configurable priority + per-source freshness window
// decides which producer's value is visible per-field.
//
// Default priority is NMEA2000 -> NmeaWifi -> SignalK -> Demo (the physical
// bus wins when present, falling back to discovered SignalK).
//
// Pure C++ - no Arduino deps so test/test_boat_data/ runs on the host.

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace boat {

enum class SourceKind : uint8_t {
    None = 0,
    Demo,
    SignalK,
    NmeaWifi,
    Nmea2000,
    COUNT,
};

struct Field {
    double value = NAN;
    uint32_t updated_ms = 0;
    SourceKind source = SourceKind::None;
};

struct Snapshot {
    // navigation
    Field lat_deg;
    Field lon_deg;
    Field sog_mps;
    Field stw_mps;
    Field cog_true_rad;
    Field heading_true_rad;

    // wind
    Field awa_rad;
    Field aws_mps;
    Field twa_rad;
    Field tws_mps;

    // depth/sea
    Field depth_m;
    Field water_temp_k;

    // electrical/tanks
    Field battery_v;
    Field battery_soc;
    Field tank_fuel;
    Field tank_water;

    // route
    Field xte_m;
    Field cts_rad;
    Field btw_rad;
    Field dtw_m;
    Field vmg_mps;

    // autopilot
    Field autopilot_target_rad;
    char autopilot_state[16] = {0};
    uint32_t autopilot_state_updated_ms = 0;
    SourceKind autopilot_state_source = SourceKind::None;

    // current/tide
    Field current_set_rad;
    Field current_drift_mps;
};

struct Priority {
    // Ranked highest priority first. Entries past the first None are
    // ignored. Default: physical bus -> WiFi NMEA -> SignalK -> Demo.
    SourceKind order[5] = {
        SourceKind::Nmea2000,
        SourceKind::NmeaWifi,
        SourceKind::SignalK,
        SourceKind::Demo,
        SourceKind::None,
    };
};

struct Timeouts {
    uint32_t nmea2000_ms = 2000;
    uint32_t nmea_wifi_ms = 3000;
    uint32_t signalk_ms = 10000;
    uint32_t demo_ms = 60000;
};

// Configuration setters/getters. Thread-safe.
void set_priority(const Priority &p);
Priority get_priority();
void set_timeouts(const Timeouts &t);
Timeouts get_timeouts();

// Publish a scalar from `src` into `field` on the global snapshot, at
// time `now_ms`. Returns true if the write was accepted (higher or equal
// priority, or current owner is stale). False if a higher-priority,
// still-fresh source owns the field.
//
// `field` is a pointer-to-member into Snapshot for type-safe routing
// without an intermediate metric-id enum. Example:
//   boat::publish(&boat::Snapshot::sog_mps, boat::SourceKind::NmeaWifi,
//                 millis(), 4.13);
bool publish(Field Snapshot::*field, SourceKind src, uint32_t now_ms, double value);

// Publish the autopilot state string. Same priority rules.
bool publish_autopilot_state(SourceKind src, uint32_t now_ms, const char *state);

// Atomic snapshot read for renderers.
void copy_snapshot(Snapshot &out);

// Reset the global snapshot (all fields cleared to NaN/None). Used by
// tests and by `boat-reset` console command.
void reset_all();

// Freshness helpers - pure, no global state needed.
bool fresh(const Field &f, uint32_t now_ms, uint32_t timeout_ms);
double value_or_nan(const Field &f, uint32_t now_ms, uint32_t timeout_ms);

// Lookup a source's configured freshness timeout from a Timeouts struct.
uint32_t timeout_for(const Timeouts &t, SourceKind src);

// Priority rank, 0 = highest. Returns 255 for SourceKind::None or
// sources missing from the priority list.
uint8_t rank_of(const Priority &p, SourceKind src);

// Pure decision helper - publish() uses this internally and tests use it
// directly. Returns true if a publish from `incoming` should overwrite a
// field currently owned by `current` at time `now_ms`.
bool should_accept(SourceKind incoming, SourceKind current,
                   uint32_t current_updated_ms, uint32_t now_ms,
                   const Priority &p, const Timeouts &t);

// Const-ref helpers for the human-readable source name (for logs/CLI).
const char *source_name(SourceKind s);

// ---- spec 12 §3 freshness + smoothing -----------------------------------
//
// EMA filters for scalars + sin/cos filters for angles. The instrument
// channels (wind, heading, speed) each have a response setting that
// translates to a time-constant tau:
//
//   fast    -> tau =  300 ms   (raw sensor follow)
//   normal  -> tau = 1500 ms
//   smooth  -> tau = 5000 ms   (heavy damping, marine instrument feel)
//
// All accessors are pure read-throughs; the smoothing state advances
// only when the underlying field updates (i.e. publish() lands a fresh
// value). NaN inputs preserve the last good output, so a stale source
// doesn't snap the gauge to zero.

enum class Response : uint8_t { Fast = 0, Normal, Smooth };

void set_wind_response(Response r);
void set_heading_response(Response r);
void set_speed_response(Response r);
Response wind_response();
Response heading_response();
Response speed_response();

// Smoothed accessors, in human-display units.
double heading_smoothed_deg();   // 0..360
double cog_smoothed_deg();       // 0..360
double awa_smoothed_deg();       // -180..180 (port negative, stbd positive)
double aws_smoothed_kn();
double twa_smoothed_deg();
double tws_smoothed_kn();
double sog_smoothed_kn();
double stw_smoothed_kn();
double depth_smoothed_m();

// Pure helpers (for host tests). Returns the new smoothed scalar
// given previous output, new sample, dt (ms), and tau (ms). NaN-safe:
// if `prev` is NaN, returns `sample`; if `sample` is NaN, returns
// `prev` unchanged.
double ema_step(double prev, double sample, uint32_t dt_ms, uint32_t tau_ms);

// Pure helper: smooth an angle (radians) using sin/cos EMAs. `prev_sin`
// / `prev_cos` are kept by the caller. Result rad in (-pi, pi].
struct AngleEma {
    double s = 0.0;  // smoothed sin
    double c = 0.0;  // smoothed cos
    bool init = false;
};
double angle_ema_step(AngleEma &state, double sample_rad,
                      uint32_t dt_ms, uint32_t tau_ms);

}  // namespace boat
