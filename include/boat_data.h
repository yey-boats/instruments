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

    // performance / polar targets (e.g. signalk-polar-performance). NaN when
    // no polar source is present; consumers fall back to empirical estimates.
    Field beat_angle_rad;  // optimal upwind TWA off bow (performance.beatAngle)
    Field gybe_angle_rad;  // optimal downwind TWA off bow (performance.gybeAngle)

    // depth/sea
    Field depth_m;
    Field depth_keel_m;
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

    // steering
    Field rudder_angle_rad;

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
        SourceKind::Nmea2000, SourceKind::NmeaWifi, SourceKind::SignalK,
        SourceKind::Demo,     SourceKind::None,
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
bool should_accept(SourceKind incoming, SourceKind current, uint32_t current_updated_ms,
                   uint32_t now_ms, const Priority &p, const Timeouts &t);

// Const-ref helpers for the human-readable source name (for logs/CLI).
const char *source_name(SourceKind s);

// Flat, source-neutral render view. This is the per-frame snapshot the UI
// reads (label(v.awa) etc.) - cheap flat doubles resolved once from the
// fused Snapshot rather than every screen re-running freshness checks. It
// replaced sk::Data: the SignalK parser fills one transiently (ingest), and
// boat::current_view() composes one from the fused Snapshot (render). Field
// names match the historical sk::Data so consumers read v.awa, v.headingTrue.
struct View {
    // navigation
    double lat = NAN, lon = NAN;
    double sog = NAN;          // speed over ground, m/s
    double stw = NAN;          // speed through water, m/s
    double cogTrue = NAN;      // course over ground (true), rad
    double headingTrue = NAN;  // true heading, rad

    // environment / wind
    double awa = NAN;        // apparent wind angle, rad
    double aws = NAN;        // apparent wind speed, m/s
    double twa = NAN;        // true wind angle, rad
    double tws = NAN;        // true wind speed, m/s
    double depth = NAN;      // m below transducer
    double depthKeel = NAN;  // m below keel
    double waterTemp = NAN;  // K

    // performance / polar targets (NaN when no polar source)
    double beatAngle = NAN;  // optimal upwind TWA off bow, rad
    double gybeAngle = NAN;  // optimal downwind TWA off bow, rad

    // electrical & tanks
    double battVoltage = NAN;  // V
    double battSoc = NAN;      // 0..1
    double tankFuel = NAN;     // 0..1
    double tankWater = NAN;    // 0..1

    // routing / steering
    double xte = NAN;          // cross-track error, m (+ = right of track)
    double cts = NAN;          // course to steer, rad
    double btw = NAN;          // bearing to waypoint, rad
    double dtw = NAN;          // distance to waypoint, m
    double vmg = NAN;          // velocity made good, m/s
    double apTargetHdg = NAN;  // autopilot target heading, rad
    double rudder = NAN;       // rudder angle, rad (+ = starboard helm)
    char apState[16] = {0};    // autopilot state string

    // current / tide
    double currentSetTrue = NAN;  // true direction current flows toward, rad
    double currentDrift = NAN;    // current speed, m/s

    // SignalK WS link-state (NOT boat metrics - filled by the WS layer, used
    // by the stall classifier; carried here so the UI has one struct to read).
    uint32_t lastUpdateMs = 0;
    uint32_t connectedSinceMs = 0;
    uint32_t wsLastFrameMs = 0;
    bool connected = false;
};

}  // namespace boat
