#include "widget_data_resolver.h"

#include <math.h>
#include <string.h>

namespace widget_data {

namespace {

// Table-driven mapping. Each entry: alias OR SK path -> offset into
// boat::View (relative to a Data * base). offsetof() doesn't work
// cleanly with C-style strings, so we use a small dispatch table.

struct NumEntry {
    const char *path;
    double (*read)(const boat::View &);
};

double r_sog(const boat::View &d) {
    return d.sog;
}
double r_stw(const boat::View &d) {
    return d.stw;
}
double r_cog(const boat::View &d) {
    return d.cogTrue;
}
double r_hdg(const boat::View &d) {
    return d.headingTrue;
}
double r_aws(const boat::View &d) {
    return d.aws;
}
double r_awa(const boat::View &d) {
    return d.awa;
}
double r_tws(const boat::View &d) {
    return d.tws;
}
double r_twa(const boat::View &d) {
    return d.twa;
}
double r_depth(const boat::View &d) {
    return d.depth;
}
double r_waterTemp(const boat::View &d) {
    return d.waterTemp;
}
double r_battV(const boat::View &d) {
    return d.battVoltage;
}
double r_battSoc(const boat::View &d) {
    return d.battSoc;
}
double r_xte(const boat::View &d) {
    return d.xte;
}
double r_btw(const boat::View &d) {
    return d.btw;
}
double r_dtw(const boat::View &d) {
    return d.dtw;
}
double r_vmg(const boat::View &d) {
    return d.vmg;
}
double r_apTarget(const boat::View &d) {
    return d.apTargetHdg;
}
double r_lat(const boat::View &d) {
    return d.lat;
}
double r_lon(const boat::View &d) {
    return d.lon;
}
double r_hdgMag(const boat::View &d) {
    return d.headingMag;
}
double r_variation(const boat::View &d) {
    return d.variation;
}
double r_roll(const boat::View &d) {
    return d.roll;
}
double r_pitch(const boat::View &d) {
    return d.pitch;
}
double r_rot(const boat::View &d) {
    return d.rateOfTurn;
}
double r_outTemp(const boat::View &d) {
    return d.outsideTemp;
}
double r_outPressure(const boat::View &d) {
    return d.outsidePressure;
}
double r_humidity(const boat::View &d) {
    return d.humidity;
}
double r_battCurrent(const boat::View &d) {
    return d.battCurrent;
}
double r_battTemp(const boat::View &d) {
    return d.battTemp;
}
double r_engineRevs(const boat::View &d) {
    return d.engineRevs;
}
double r_engineCoolant(const boat::View &d) {
    return d.engineCoolantTemp;
}
double r_engineOilP(const boat::View &d) {
    return d.engineOilPressure;
}
double r_engineFuel(const boat::View &d) {
    return d.engineFuelRate;
}
double r_tripLog(const boat::View &d) {
    return d.tripLog;
}
double r_totalLog(const boat::View &d) {
    return d.totalLog;
}

const NumEntry NUM_TABLE[] = {
    // Local aliases (spec 19 §"Data Path Resolution")
    {"boat.sog", r_sog},
    {"boat.stw", r_stw},
    {"boat.cogTrue", r_cog},
    {"boat.headingTrue", r_hdg},
    {"boat.aws", r_aws},
    {"boat.awa", r_awa},
    {"boat.tws", r_tws},
    {"boat.twa", r_twa},
    {"boat.depth", r_depth},
    {"boat.waterTemp", r_waterTemp},
    {"boat.batteryVoltage", r_battV},
    {"boat.batterySoc", r_battSoc},
    {"boat.xte", r_xte},
    {"boat.btw", r_btw},
    {"boat.dtw", r_dtw},
    {"boat.vmg", r_vmg},
    {"boat.autopilotTarget", r_apTarget},
    {"boat.lat", r_lat},
    {"boat.lon", r_lon},
    // Raw SignalK paths the parser already knows
    {"navigation.speedOverGround", r_sog},
    {"navigation.speedThroughWater", r_stw},
    {"navigation.courseOverGroundTrue", r_cog},
    {"navigation.headingTrue", r_hdg},
    {"environment.wind.speedApparent", r_aws},
    {"environment.wind.angleApparent", r_awa},
    {"environment.wind.speedTrue", r_tws},
    {"environment.wind.angleTrueWater", r_twa},
    {"environment.depth.belowTransducer", r_depth},
    {"environment.water.temperature", r_waterTemp},
    {"electrical.batteries.house.voltage", r_battV},
    {"electrical.batteries.house.stateOfCharge", r_battSoc},
    {"navigation.courseRhumbline.crossTrackError", r_xte},
    {"navigation.courseRhumbline.nextPoint.bearingTrue", r_btw},
    {"navigation.courseRhumbline.nextPoint.distance", r_dtw},
    {"navigation.courseRhumbline.velocityMadeGood", r_vmg},
    {"steering.autopilot.target.headingTrue", r_apTarget},
    // Coverage wave (all raw SI, same convention as above). Aliases first.
    {"boat.headingMag", r_hdgMag},
    {"boat.variation", r_variation},
    {"boat.roll", r_roll},
    {"boat.pitch", r_pitch},
    {"navigation.headingMagnetic", r_hdgMag},
    {"navigation.magneticVariation", r_variation},
    {"navigation.attitude.roll", r_roll},
    {"navigation.attitude.pitch", r_pitch},
    {"navigation.rateOfTurn", r_rot},
    {"navigation.trip.log", r_tripLog},
    {"navigation.log", r_totalLog},
    {"environment.outside.temperature", r_outTemp},
    {"environment.outside.pressure", r_outPressure},
    {"environment.outside.humidity", r_humidity},
    {"environment.outside.relativeHumidity", r_humidity},
    {"electrical.batteries.house.current", r_battCurrent},
    {"electrical.batteries.house.temperature", r_battTemp},
    {"propulsion.main.revolutions", r_engineRevs},
    {"propulsion.main.temperature", r_engineCoolant},
    {"propulsion.main.oilPressure", r_engineOilP},
    {"propulsion.main.fuel.rate", r_engineFuel},
};

constexpr size_t NUM_TABLE_COUNT = sizeof(NUM_TABLE) / sizeof(NUM_TABLE[0]);

}  // namespace

double resolve_numeric(const char *path, const boat::View &d) {
    if (!path || !*path) return NAN;
    for (size_t i = 0; i < NUM_TABLE_COUNT; ++i) {
        if (strcmp(NUM_TABLE[i].path, path) == 0) {
            return NUM_TABLE[i].read(d);
        }
    }
    return NAN;
}

double resolve_numeric(const char *path, const boat::View &d, const sk::PathStore *store) {
    double v = resolve_numeric(path, d);                     // typed alias/known-field
    if (!isnan(v)) return v;                                 // typed field wins
    if (store && store->has(path)) return store->get(path);  // step 3: dynamic store
    return NAN;
}

bool captureDynamic(const char *path, double value, sk::PathStore &store) {
    return store.set(path, value);
}

bool resolve_string(const char *path, const boat::View &d, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!path) return false;
    if (strcmp(path, "boat.autopilotState") == 0 || strcmp(path, "steering.autopilot.state") == 0) {
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
    if (strcmp(path, "boat.autopilotState") == 0 || strcmp(path, "steering.autopilot.state") == 0)
        return true;
    return false;
}

}  // namespace widget_data
