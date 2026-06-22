#include "signalk_parser.h"

#include <string.h>

namespace sk {

#ifdef DBG_PERF_COUNTERS
static uint32_t g_parsed_count = 0;
uint32_t takeParsedCount() {
    uint32_t v = g_parsed_count;
    g_parsed_count = 0;
    return v;
}
#endif

static bool isNumeric(JsonVariant v) {
    return v.is<double>() || v.is<float>() || v.is<int>() || v.is<long>() || v.is<unsigned int>() ||
           v.is<long long>();
}

static double asDouble(JsonVariant v, double def = NAN) {
    if (v.isNull()) return def;
    if (isNumeric(v)) return v.as<double>();
    return def;
}

static bool endsWith(const char *s, const char *suffix) {
    size_t sl = strlen(s);
    size_t suf = strlen(suffix);
    return sl >= suf && strcmp(s + sl - suf, suffix) == 0;
}

void applyValue(const char *path, JsonVariant val, boat::View &out) {
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
               strcmp(path, "environment.depth.belowSurface") == 0) {
        out.depth = asDouble(val, out.depth);
    } else if (strcmp(path, "environment.depth.belowKeel") == 0) {
        out.depthKeel = asDouble(val, out.depthKeel);
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
    } else if (strcmp(path, "performance.beatAngle") == 0) {
        out.beatAngle = asDouble(val, out.beatAngle);
    } else if (strcmp(path, "performance.gybeAngle") == 0) {
        out.gybeAngle = asDouble(val, out.gybeAngle);
    } else if (strcmp(path, "steering.autopilot.target.headingTrue") == 0) {
        out.apTargetHdg = asDouble(val, out.apTargetHdg);
    } else if (strcmp(path, "steering.rudderAngle") == 0) {
        out.rudder = asDouble(val, out.rudder);
    } else if (strcmp(path, "environment.current.setTrue") == 0 ||
               strcmp(path, "environment.current.drift.setTrue") == 0) {
        out.currentSetTrue = asDouble(val, out.currentSetTrue);
    } else if (strcmp(path, "environment.current.drift") == 0 ||
               strcmp(path, "environment.current.speed") == 0) {
        out.currentDrift = asDouble(val, out.currentDrift);
    } else if (strcmp(path, "steering.autopilot.state") == 0) {
        const char *s = val.as<const char *>();
        if (s) {
            strncpy(out.apState, s, sizeof(out.apState) - 1);
            out.apState[sizeof(out.apState) - 1] = 0;
        }
    }
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn);

int applyDelta(const char *json, size_t len, boat::View &out, ArduinoJson::Allocator *alloc,
               PathStore *dyn) {
    // alloc==nullptr -> default (internal heap) allocator. The device
    // build passes &yeyboats::psram_json so 1+ Hz SK deltas don't churn
    // the tiny internal heap (largest free block was ~7 KB at idle).
    // Host tests pass nullptr and use the default.
    if (alloc) {
        JsonDocument doc(alloc);
        return apply_delta_impl(json, len, out, doc, dyn);
    }
    JsonDocument doc;
    return apply_delta_impl(json, len, out, doc, dyn);
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn) {
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
#ifdef DBG_PERF_COUNTERS
            ++g_parsed_count;
#endif
            applyValue(p, v["value"], out);
            // Mirror numeric deltas into the dynamic store so authored fields
            // can render arbitrary paths by string (typed boat::View still drives
            // the built-in screens).
            if (dyn && isNumeric(v["value"])) dyn->set(p, v["value"].as<double>());
            ++count;
        }
    }
    return count;
}

const char *classifyStatus(bool connected, uint32_t lastUpdateMs, uint32_t connectedSinceMs,
                           uint32_t wsLastFrameMs, uint32_t nowMs, uint32_t stallMs,
                           uint32_t noDataMs) {
    if (!connected) return "disconnected";
    // Pick the freshest of the three signals as the staleness reference.
    // wsLastFrameMs catches link activity that doesn't tick lastUpdateMs
    // (hello, subscription acks, envelope-only deltas) so a healthy but
    // value-quiet server doesn't trip the alarm. connectedSinceMs is the
    // warmup floor for fresh (re)connects. signalk.cpp resets all three
    // on disconnect so stale pre-disconnect timestamps can't leak across.
    uint32_t ref = 0;
    auto bump = [&](uint32_t t) {
        if (!t) return;
        if (!ref || (int32_t)(t - ref) > 0) ref = t;
    };
    bump(lastUpdateMs);
    bump(wsLastFrameMs);
    bump(connectedSinceMs);
    if (ref == 0) return "live";
    uint32_t ago = nowMs - ref;
    if (ago > stallMs) return "stalled";
    // Link is up and frames are flowing, but if no value-bearing delta
    // has ever landed after the warmup window the server has no
    // producers - surface that as a distinct state so the UI can show
    // "no data" instead of silent dashes.
    if (lastUpdateMs == 0 && connectedSinceMs != 0 &&
        (uint32_t)(nowMs - connectedSinceMs) > noDataMs)
        return "no-data";
    return "live";
}

}  // namespace sk
