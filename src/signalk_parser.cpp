#include "signalk_parser.h"

#include <string.h>

namespace sk {

static double asDouble(JsonVariant v, double def = NAN) {
    if (v.isNull()) return def;
    if (v.is<double>() || v.is<float>() || v.is<int>() || v.is<long>() || v.is<unsigned int>() ||
        v.is<long long>()) {
        return v.as<double>();
    }
    return def;
}

static bool endsWith(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t suf = strlen(suffix);
    return sl >= suf && strcmp(s + sl - suf, suffix) == 0;
}

void applyValue(const char *path, JsonVariant val, Data &out) {
    if (!path) return;

    if (strcmp(path, "navigation.position") == 0) {
        if (val.is<JsonObject>()) {
            out.lat = asDouble(val["latitude"], out.lat);
            out.lon = asDouble(val["longitude"], out.lon);
        }
    } else if (strcmp(path, "navigation.speedOverGround") == 0) {
        out.sog = asDouble(val, out.sog);
    } else if (strcmp(path, "navigation.speedThroughWater") == 0) {
        out.stw = asDouble(val, out.stw);
    } else if (strcmp(path, "navigation.courseOverGroundTrue") == 0) {
        out.cogTrue = asDouble(val, out.cogTrue);
    } else if (strcmp(path, "navigation.headingTrue") == 0) {
        out.headingTrue = asDouble(val, out.headingTrue);
    } else if (strcmp(path, "environment.wind.angleApparent") == 0) {
        out.awa = asDouble(val, out.awa);
    } else if (strcmp(path, "environment.wind.speedApparent") == 0) {
        out.aws = asDouble(val, out.aws);
    } else if (strcmp(path, "environment.wind.angleTrueWater") == 0 ||
               strcmp(path, "environment.wind.angleTrueGround") == 0) {
        out.twa = asDouble(val, out.twa);
    } else if (strcmp(path, "environment.wind.speedTrue") == 0) {
        out.tws = asDouble(val, out.tws);
    } else if (strcmp(path, "environment.depth.belowTransducer") == 0 ||
               strcmp(path, "environment.depth.belowKeel") == 0 ||
               strcmp(path, "environment.depth.belowSurface") == 0) {
        out.depth = asDouble(val, out.depth);
    } else if (strcmp(path, "environment.water.temperature") == 0) {
        out.waterTemp = asDouble(val, out.waterTemp);
    } else if (strncmp(path, "electrical.batteries.", 21) == 0) {
        if (endsWith(path, ".voltage")) {
            out.battVoltage = asDouble(val, out.battVoltage);
        } else if (endsWith(path, ".stateOfCharge")) {
            out.battSoc = asDouble(val, out.battSoc);
        }
    } else if (strncmp(path, "tanks.fuel.", 11) == 0 && endsWith(path, ".currentLevel")) {
        out.tankFuel = asDouble(val, out.tankFuel);
    } else if (strncmp(path, "tanks.freshWater.", 17) == 0 && endsWith(path, ".currentLevel")) {
        out.tankWater = asDouble(val, out.tankWater);
    } else if (strcmp(path, "navigation.courseRhumbline.crossTrackError") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.crossTrackError") == 0) {
        out.xte = asDouble(val, out.xte);
    } else if (strcmp(path, "navigation.courseRhumbline.bearingTrackTrue") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.bearingTrackTrue") == 0) {
        out.cts = asDouble(val, out.cts);
    } else if (strcmp(path, "navigation.courseRhumbline.nextPoint.bearingTrue") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.nextPoint.bearingTrue") == 0) {
        out.btw = asDouble(val, out.btw);
    } else if (strcmp(path, "navigation.courseRhumbline.nextPoint.distance") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.nextPoint.distance") == 0) {
        out.dtw = asDouble(val, out.dtw);
    } else if (strcmp(path, "navigation.courseRhumbline.velocityMadeGood") == 0 ||
               strcmp(path, "performance.velocityMadeGood") == 0) {
        out.vmg = asDouble(val, out.vmg);
    } else if (strcmp(path, "steering.autopilot.target.headingTrue") == 0) {
        out.apTargetHdg = asDouble(val, out.apTargetHdg);
    } else if (strcmp(path, "steering.autopilot.state") == 0) {
        const char *s = val.as<const char *>();
        if (s) {
            strncpy(out.apState, s, sizeof(out.apState) - 1);
            out.apState[sizeof(out.apState) - 1] = 0;
        }
    }
}

int applyDelta(const char *json, size_t len, Data &out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) return -1;
    JsonArray updates = doc["updates"].as<JsonArray>();
    if (updates.isNull()) return 0;
    int count = 0;
    for (JsonObject upd : updates) {
        JsonArray values = upd["values"].as<JsonArray>();
        if (values.isNull()) continue;
        for (JsonObject v : values) {
            const char *p = v["path"];
            if (!p) continue;
            applyValue(p, v["value"], out);
            ++count;
        }
    }
    return count;
}

}  // namespace sk
