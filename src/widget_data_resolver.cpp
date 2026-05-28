#include "widget_data_resolver.h"

#include <math.h>
#include <string.h>

namespace widget_data {

namespace {

// Table-driven mapping. Each entry: alias OR SK path -> offset into
// sk::Data (relative to a Data * base). offsetof() doesn't work
// cleanly with C-style strings, so we use a small dispatch table.

struct NumEntry {
    const char *path;
    double (*read)(const sk::Data &);
};

double r_sog(const sk::Data &d) { return d.sog; }
double r_stw(const sk::Data &d) { return d.stw; }
double r_cog(const sk::Data &d) { return d.cogTrue; }
double r_hdg(const sk::Data &d) { return d.headingTrue; }
double r_aws(const sk::Data &d) { return d.aws; }
double r_awa(const sk::Data &d) { return d.awa; }
double r_tws(const sk::Data &d) { return d.tws; }
double r_twa(const sk::Data &d) { return d.twa; }
double r_depth(const sk::Data &d) { return d.depth; }
double r_waterTemp(const sk::Data &d) { return d.waterTemp; }
double r_battV(const sk::Data &d) { return d.battVoltage; }
double r_battSoc(const sk::Data &d) { return d.battSoc; }
double r_xte(const sk::Data &d) { return d.xte; }
double r_btw(const sk::Data &d) { return d.btw; }
double r_dtw(const sk::Data &d) { return d.dtw; }
double r_vmg(const sk::Data &d) { return d.vmg; }
double r_apTarget(const sk::Data &d) { return d.apTargetHdg; }
double r_lat(const sk::Data &d) { return d.lat; }
double r_lon(const sk::Data &d) { return d.lon; }

const NumEntry NUM_TABLE[] = {
    // Local aliases (spec 19 §"Data Path Resolution")
    {"boat.sog",              r_sog},
    {"boat.stw",              r_stw},
    {"boat.cogTrue",          r_cog},
    {"boat.headingTrue",      r_hdg},
    {"boat.aws",              r_aws},
    {"boat.awa",              r_awa},
    {"boat.tws",              r_tws},
    {"boat.twa",              r_twa},
    {"boat.depth",            r_depth},
    {"boat.waterTemp",        r_waterTemp},
    {"boat.batteryVoltage",   r_battV},
    {"boat.batterySoc",       r_battSoc},
    {"boat.xte",              r_xte},
    {"boat.btw",              r_btw},
    {"boat.dtw",              r_dtw},
    {"boat.vmg",              r_vmg},
    {"boat.autopilotTarget",  r_apTarget},
    {"boat.lat",              r_lat},
    {"boat.lon",              r_lon},
    // Raw SignalK paths the parser already knows
    {"navigation.speedOverGround",                        r_sog},
    {"navigation.speedThroughWater",                      r_stw},
    {"navigation.courseOverGroundTrue",                   r_cog},
    {"navigation.headingTrue",                            r_hdg},
    {"environment.wind.speedApparent",                    r_aws},
    {"environment.wind.angleApparent",                    r_awa},
    {"environment.wind.speedTrue",                        r_tws},
    {"environment.wind.angleTrueWater",                   r_twa},
    {"environment.depth.belowTransducer",                 r_depth},
    {"environment.water.temperature",                     r_waterTemp},
    {"electrical.batteries.house.voltage",                r_battV},
    {"electrical.batteries.house.stateOfCharge",          r_battSoc},
    {"navigation.courseRhumbline.crossTrackError",        r_xte},
    {"navigation.courseRhumbline.nextPoint.bearingTrue",  r_btw},
    {"navigation.courseRhumbline.nextPoint.distance",     r_dtw},
    {"navigation.courseRhumbline.velocityMadeGood",       r_vmg},
    {"steering.autopilot.target.headingTrue",             r_apTarget},
};

constexpr size_t NUM_TABLE_COUNT = sizeof(NUM_TABLE) / sizeof(NUM_TABLE[0]);

}  // namespace

double resolve_numeric(const char *path, const sk::Data &d) {
    if (!path || !*path) return NAN;
    for (size_t i = 0; i < NUM_TABLE_COUNT; ++i) {
        if (strcmp(NUM_TABLE[i].path, path) == 0) {
            return NUM_TABLE[i].read(d);
        }
    }
    return NAN;
}

bool resolve_string(const char *path, const sk::Data &d,
                    char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!path) return false;
    if (strcmp(path, "boat.autopilotState") == 0 ||
        strcmp(path, "steering.autopilot.state") == 0) {
        strncpy(out, d.apState, cap - 1);
        out[cap - 1] = 0;
        return out[0] != 0;
    }
    return false;
}

bool is_known(const char *path) {
    if (!path || !*path) return false;
    for (size_t i = 0; i < NUM_TABLE_COUNT; ++i) {
        if (strcmp(NUM_TABLE[i].path, path) == 0) return true;
    }
    if (strcmp(path, "boat.autopilotState") == 0 ||
        strcmp(path, "steering.autopilot.state") == 0) return true;
    return false;
}

}  // namespace widget_data
