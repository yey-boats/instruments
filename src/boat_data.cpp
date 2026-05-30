#include "boat_data.h"

#include <string.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace boat {

namespace {

Snapshot g_snap;
Priority g_prio;
Timeouts g_timeouts;

#ifdef ARDUINO
SemaphoreHandle_t g_mtx = nullptr;
void mtx_lock() {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
    if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY);
}
void mtx_unlock() {
    if (g_mtx) xSemaphoreGive(g_mtx);
}
#else
void mtx_lock() {
}
void mtx_unlock() {
}
#endif

struct Lock {
    Lock() { mtx_lock(); }
    ~Lock() { mtx_unlock(); }
};

}  // namespace

uint8_t rank_of(const Priority &p, SourceKind src) {
    if (src == SourceKind::None) return 255;
    for (uint8_t i = 0; i < sizeof(p.order) / sizeof(p.order[0]); ++i) {
        if (p.order[i] == src) return i;
        if (p.order[i] == SourceKind::None) break;
    }
    return 255;
}

uint32_t timeout_for(const Timeouts &t, SourceKind src) {
    switch (src) {
    case SourceKind::Nmea2000:
        return t.nmea2000_ms;
    case SourceKind::NmeaWifi:
        return t.nmea_wifi_ms;
    case SourceKind::SignalK:
        return t.signalk_ms;
    case SourceKind::Demo:
        return t.demo_ms;
    default:
        return 0;
    }
}

bool fresh(const Field &f, uint32_t now_ms, uint32_t timeout_ms) {
    if (f.source == SourceKind::None) return false;
    if (timeout_ms == 0) return false;
    return (now_ms - f.updated_ms) < timeout_ms;
}

double value_or_nan(const Field &f, uint32_t now_ms, uint32_t timeout_ms) {
    return fresh(f, now_ms, timeout_ms) ? f.value : NAN;
}

bool should_accept(SourceKind incoming, SourceKind current, uint32_t current_updated_ms,
                   uint32_t now_ms, const Priority &p, const Timeouts &t) {
    if (incoming == SourceKind::None) return false;
    if (current == SourceKind::None) return true;
    uint8_t r_in = rank_of(p, incoming);
    uint8_t r_cur = rank_of(p, current);
    if (r_in <= r_cur) return true;  // same or higher priority
    // Incoming is lower priority - only accept if current owner is stale.
    uint32_t to = timeout_for(t, current);
    if (to == 0) return true;
    return (now_ms - current_updated_ms) >= to;
}

const char *source_name(SourceKind s) {
    switch (s) {
    case SourceKind::None:
        return "none";
    case SourceKind::Demo:
        return "demo";
    case SourceKind::SignalK:
        return "signalk";
    case SourceKind::NmeaWifi:
        return "nmea-wifi";
    case SourceKind::Nmea2000:
        return "nmea2000";
    default:
        return "?";
    }
}

void set_priority(const Priority &p) {
    Lock _;
    g_prio = p;
}

Priority get_priority() {
    Lock _;
    return g_prio;
}

void set_timeouts(const Timeouts &t) {
    Lock _;
    g_timeouts = t;
}

Timeouts get_timeouts() {
    Lock _;
    return g_timeouts;
}

bool publish(Field Snapshot::*field, SourceKind src, uint32_t now_ms, double value) {
    Lock _;
    Field &f = g_snap.*field;
    if (!should_accept(src, f.source, f.updated_ms, now_ms, g_prio, g_timeouts)) {
        return false;
    }
    f.value = value;
    f.updated_ms = now_ms;
    f.source = src;
    return true;
}

bool publish_autopilot_state(SourceKind src, uint32_t now_ms, const char *state) {
    if (!state) return false;
    Lock _;
    if (!should_accept(src, g_snap.autopilot_state_source, g_snap.autopilot_state_updated_ms,
                       now_ms, g_prio, g_timeouts)) {
        return false;
    }
    strncpy(g_snap.autopilot_state, state, sizeof(g_snap.autopilot_state) - 1);
    g_snap.autopilot_state[sizeof(g_snap.autopilot_state) - 1] = 0;
    g_snap.autopilot_state_updated_ms = now_ms;
    g_snap.autopilot_state_source = src;
    return true;
}

void copy_snapshot(Snapshot &out) {
    Lock _;
    out = g_snap;
}

void reset_all() {
    Lock _;
    g_snap = Snapshot{};
    g_prio = Priority{};
    g_timeouts = Timeouts{};
}

// ---- spec 12 §3 smoothing ----------------------------------------------

namespace {

constexpr uint32_t TAU_FAST_MS = 300;
constexpr uint32_t TAU_NORMAL_MS = 1500;
constexpr uint32_t TAU_SMOOTH_MS = 5000;

uint32_t tau_for(Response r) {
    switch (r) {
    case Response::Fast:
        return TAU_FAST_MS;
    case Response::Normal:
        return TAU_NORMAL_MS;
    case Response::Smooth:
        return TAU_SMOOTH_MS;
    }
    return TAU_NORMAL_MS;
}

Response g_resp_wind = Response::Normal;
Response g_resp_heading = Response::Normal;
Response g_resp_speed = Response::Normal;

// Per-channel smoothing state. Scalars carry (value, last_input_ms);
// angles carry an AngleEma state + last_input_ms. The "last_input_ms"
// is the publish timestamp of the most recent SAMPLE we've consumed,
// so we only advance the filter on truly new samples.
struct ScalarState {
    double y = NAN;
    uint32_t last_sample_ms = 0;
    bool init = false;
};
struct AngleState {
    AngleEma ema;
    uint32_t last_sample_ms = 0;
};

ScalarState st_aws;
ScalarState st_tws;
ScalarState st_sog;
ScalarState st_stw;
ScalarState st_depth;
AngleState st_awa;
AngleState st_twa;
AngleState st_hdg;
AngleState st_cog;

// Pull a current sample out of the snapshot under the lock. We do not
// step the filter while holding the lock - just copy out value+ms.
void copy_field(double Snapshot::*invalid,  // unused (workaround); see below
                const Field &f, double &out_value, uint32_t &out_ms) {
    (void)invalid;
    out_value = f.value;
    out_ms = f.updated_ms;
}

double step_scalar(ScalarState &s, const Field &f, Response r) {
    double sample;
    uint32_t sample_ms;
    {
        Lock _;
        sample = f.value;
        sample_ms = f.updated_ms;
    }
    if (sample_ms == s.last_sample_ms) {
        // No new input; return last smoothed (or NaN if never seen).
        return s.y;
    }
    uint32_t dt_ms = s.init ? (sample_ms - s.last_sample_ms) : 0;
    s.y = ema_step(s.y, sample, dt_ms, tau_for(r));
    s.last_sample_ms = sample_ms;
    s.init = true;
    return s.y;
}

double step_angle(AngleState &s, const Field &f, Response r) {
    double sample_rad;
    uint32_t sample_ms;
    {
        Lock _;
        sample_rad = f.value;
        sample_ms = f.updated_ms;
    }
    if (sample_ms == s.last_sample_ms) {
        if (!s.ema.init) return NAN;
        return atan2(s.ema.s, s.ema.c);
    }
    uint32_t dt_ms = s.ema.init ? (sample_ms - s.last_sample_ms) : 0;
    double out = angle_ema_step(s.ema, sample_rad, dt_ms, tau_for(r));
    s.last_sample_ms = sample_ms;
    return out;
}

}  // namespace

void set_wind_response(Response r) {
    g_resp_wind = r;
}
void set_heading_response(Response r) {
    g_resp_heading = r;
}
void set_speed_response(Response r) {
    g_resp_speed = r;
}
Response wind_response() {
    return g_resp_wind;
}
Response heading_response() {
    return g_resp_heading;
}
Response speed_response() {
    return g_resp_speed;
}

double ema_step(double prev, double sample, uint32_t dt_ms, uint32_t tau_ms) {
    if (isnan(sample)) return prev;
    if (isnan(prev)) return sample;
    if (dt_ms == 0 || tau_ms == 0) return sample;
    // First-order IIR: alpha = 1 - exp(-dt/tau). For tiny dt/tau this
    // is roughly dt/tau; we use expm1 for precision near zero.
    double alpha = 1.0 - exp(-((double)dt_ms / (double)tau_ms));
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    return prev + alpha * (sample - prev);
}

double angle_ema_step(AngleEma &state, double sample_rad, uint32_t dt_ms, uint32_t tau_ms) {
    if (isnan(sample_rad)) {
        if (!state.init) return NAN;
        return atan2(state.s, state.c);
    }
    double sx = sin(sample_rad);
    double cx = cos(sample_rad);
    if (!state.init) {
        state.s = sx;
        state.c = cx;
        state.init = true;
        return sample_rad;
    }
    double alpha = (dt_ms == 0 || tau_ms == 0) ? 1.0 : 1.0 - exp(-((double)dt_ms / (double)tau_ms));
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    state.s += alpha * (sx - state.s);
    state.c += alpha * (cx - state.c);
    return atan2(state.s, state.c);
}

// Unit-converted public accessors.

double aws_smoothed_kn() {
    Snapshot s;
    copy_snapshot(s);
    double v = step_scalar(st_aws, s.aws_mps, g_resp_wind);
    return isnan(v) ? NAN : v * 1.943844;
}
double tws_smoothed_kn() {
    Snapshot s;
    copy_snapshot(s);
    double v = step_scalar(st_tws, s.tws_mps, g_resp_wind);
    return isnan(v) ? NAN : v * 1.943844;
}
double sog_smoothed_kn() {
    Snapshot s;
    copy_snapshot(s);
    double v = step_scalar(st_sog, s.sog_mps, g_resp_speed);
    return isnan(v) ? NAN : v * 1.943844;
}
double stw_smoothed_kn() {
    Snapshot s;
    copy_snapshot(s);
    double v = step_scalar(st_stw, s.stw_mps, g_resp_speed);
    return isnan(v) ? NAN : v * 1.943844;
}
double depth_smoothed_m() {
    Snapshot s;
    copy_snapshot(s);
    return step_scalar(st_depth, s.depth_m, g_resp_speed);
}

static double rad_to_deg_0_360(double r) {
    if (isnan(r)) return NAN;
    double d = r * (180.0 / M_PI);
    while (d < 0)
        d += 360.0;
    while (d >= 360.0)
        d -= 360.0;
    return d;
}
static double rad_to_deg_signed(double r) {
    if (isnan(r)) return NAN;
    return r * (180.0 / M_PI);
}

double heading_smoothed_deg() {
    Snapshot s;
    copy_snapshot(s);
    return rad_to_deg_0_360(step_angle(st_hdg, s.heading_true_rad, g_resp_heading));
}
double cog_smoothed_deg() {
    Snapshot s;
    copy_snapshot(s);
    return rad_to_deg_0_360(step_angle(st_cog, s.cog_true_rad, g_resp_heading));
}
double awa_smoothed_deg() {
    Snapshot s;
    copy_snapshot(s);
    return rad_to_deg_signed(step_angle(st_awa, s.awa_rad, g_resp_wind));
}
double twa_smoothed_deg() {
    Snapshot s;
    copy_snapshot(s);
    return rad_to_deg_signed(step_angle(st_twa, s.twa_rad, g_resp_wind));
}

}  // namespace boat
