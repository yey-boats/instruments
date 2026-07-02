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

void applyValue(const char *path, JsonVariant val, boat::View &out, boat::FieldMask *touched) {
    if (!path) return;

    // Assign `dst` and mark its FieldId bit ONLY when the delta actually
    // carried a numeric value (null / wrong-type leaves the field AND the
    // mask untouched, preserving the old asDouble(v, old) semantics).
    auto num = [&](double &dst, boat::FieldId id) {
        if (!isNumeric(val)) return;
        dst = val.as<double>();
        if (touched) *touched |= boat::field_bit(id);
    };
    using FI = boat::FieldId;

    if (strcmp(path, "navigation.position") == 0) {
        if (val.is<JsonObject>()) {
            JsonVariant la = val["latitude"];
            JsonVariant lo = val["longitude"];
            if (isNumeric(la)) {
                out.lat = la.as<double>();
                if (touched) *touched |= boat::field_bit(FI::Lat);
            }
            if (isNumeric(lo)) {
                out.lon = lo.as<double>();
                if (touched) *touched |= boat::field_bit(FI::Lon);
            }
        }
    } else if (strcmp(path, "navigation.speedOverGround") == 0) {
        num(out.sog, FI::Sog);
    } else if (strcmp(path, "navigation.speedThroughWater") == 0) {
        num(out.stw, FI::Stw);
    } else if (strcmp(path, "navigation.courseOverGroundTrue") == 0) {
        num(out.cogTrue, FI::CogTrue);
    } else if (strcmp(path, "navigation.headingTrue") == 0) {
        num(out.headingTrue, FI::HeadingTrue);
    } else if (strcmp(path, "environment.wind.angleApparent") == 0) {
        num(out.awa, FI::Awa);
    } else if (strcmp(path, "environment.wind.speedApparent") == 0) {
        num(out.aws, FI::Aws);
    } else if (strcmp(path, "environment.wind.angleTrueWater") == 0 ||
               strcmp(path, "environment.wind.angleTrueGround") == 0) {
        num(out.twa, FI::Twa);
    } else if (strcmp(path, "environment.wind.speedTrue") == 0) {
        num(out.tws, FI::Tws);
    } else if (strcmp(path, "environment.depth.belowTransducer") == 0 ||
               strcmp(path, "environment.depth.belowSurface") == 0) {
        num(out.depth, FI::Depth);
    } else if (strcmp(path, "environment.depth.belowKeel") == 0) {
        num(out.depthKeel, FI::DepthKeel);
    } else if (strcmp(path, "environment.water.temperature") == 0) {
        num(out.waterTemp, FI::WaterTemp);
    } else if (strcmp(path, "environment.outside.temperature") == 0) {
        num(out.outsideTemp, FI::OutsideTemp);
    } else if (strcmp(path, "environment.outside.pressure") == 0) {
        num(out.outsidePressure, FI::OutsidePressure);
    } else if (strcmp(path, "environment.outside.humidity") == 0 ||
               strcmp(path, "environment.outside.relativeHumidity") == 0) {
        // Spec canonical is relativeHumidity; `humidity` is the widely-emitted
        // legacy alias (both are a 0..1 ratio).
        num(out.humidity, FI::Humidity);
    } else if (strcmp(path, "navigation.attitude") == 0) {
        // Object value {roll, pitch, yaw} — parsed like navigation.position.
        // yaw duplicates headingTrue, so only roll/pitch land on typed fields.
        if (val.is<JsonObject>()) {
            JsonVariant ro = val["roll"];
            JsonVariant pi = val["pitch"];
            if (isNumeric(ro)) {
                out.roll = ro.as<double>();
                if (touched) *touched |= boat::field_bit(FI::Roll);
            }
            if (isNumeric(pi)) {
                out.pitch = pi.as<double>();
                if (touched) *touched |= boat::field_bit(FI::Pitch);
            }
        }
    } else if (strcmp(path, "navigation.attitude.roll") == 0) {
        num(out.roll, FI::Roll);  // some producers emit the leaf paths
    } else if (strcmp(path, "navigation.attitude.pitch") == 0) {
        num(out.pitch, FI::Pitch);
    } else if (strcmp(path, "navigation.rateOfTurn") == 0) {
        num(out.rateOfTurn, FI::RateOfTurn);
    } else if (strcmp(path, "navigation.trip.log") == 0) {
        num(out.tripLog, FI::TripLog);
    } else if (strcmp(path, "navigation.log") == 0) {
        num(out.totalLog, FI::TotalLog);
    } else if (strcmp(path, "navigation.headingMagnetic") == 0) {
        num(out.headingMag, FI::HeadingMag);
    } else if (strcmp(path, "navigation.magneticVariation") == 0) {
        num(out.variation, FI::Variation);
    } else if (strncmp(path, "electrical.batteries.", 21) == 0) {
        if (endsWith(path, ".voltage")) {
            num(out.battVoltage, FI::BattVoltage);
        } else if (endsWith(path, ".stateOfCharge")) {
            num(out.battSoc, FI::BattSoc);
        } else if (endsWith(path, ".current")) {
            num(out.battCurrent, FI::BattCurrent);
        } else if (endsWith(path, ".temperature")) {
            num(out.battTemp, FI::BattTemp);
        }
    } else if (strncmp(path, "propulsion.", 11) == 0) {
        // Instance segment is arbitrary ("main"/"port"/"0"); prefix-match like
        // electrical.batteries.* — first/primary engine wins the typed fields
        // (multi-engine installs read the others via the dynamic PathStore).
        if (endsWith(path, ".revolutions")) {
            num(out.engineRevs, FI::EngineRpm);
        } else if (endsWith(path, ".temperature")) {
            num(out.engineCoolantTemp, FI::EngineCoolantTemp);
        } else if (endsWith(path, ".oilPressure")) {
            num(out.engineOilPressure, FI::EngineOilPressure);
        } else if (endsWith(path, ".fuel.rate")) {
            num(out.engineFuelRate, FI::EngineFuelRate);
        }
    } else if (strncmp(path, "tanks.fuel.", 11) == 0 && endsWith(path, ".currentLevel")) {
        num(out.tankFuel, FI::TankFuel);
    } else if (strncmp(path, "tanks.freshWater.", 17) == 0 && endsWith(path, ".currentLevel")) {
        num(out.tankWater, FI::TankWater);
    } else if (strcmp(path, "navigation.courseRhumbline.crossTrackError") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.crossTrackError") == 0) {
        num(out.xte, FI::Xte);
    } else if (strcmp(path, "navigation.courseRhumbline.bearingTrackTrue") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.bearingTrackTrue") == 0) {
        num(out.cts, FI::Cts);
    } else if (strcmp(path, "navigation.courseRhumbline.nextPoint.bearingTrue") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.nextPoint.bearingTrue") == 0) {
        num(out.btw, FI::Btw);
    } else if (strcmp(path, "navigation.courseRhumbline.nextPoint.distance") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.nextPoint.distance") == 0) {
        num(out.dtw, FI::Dtw);
    } else if (strcmp(path, "navigation.courseRhumbline.velocityMadeGood") == 0 ||
               strcmp(path, "navigation.courseGreatCircle.velocityMadeGood") == 0) {
        // VMG toward the next waypoint (route VMG). Kept on `vmg`.
        num(out.vmg, FI::Vmg);
    } else if (strcmp(path, "performance.velocityMadeGood") == 0) {
        // Wind/polar VMG (made good to windward) — a DISTINCT metric from the
        // waypoint VMG above; surfaced on its own field/readout.
        num(out.vmgWind, FI::VmgWind);
    } else if (strcmp(path, "performance.beatAngle") == 0) {
        num(out.beatAngle, FI::BeatAngle);
    } else if (strcmp(path, "performance.gybeAngle") == 0) {
        num(out.gybeAngle, FI::GybeAngle);
    } else if (strcmp(path, "steering.autopilot.target.headingTrue") == 0) {
        num(out.apTargetHdg, FI::ApTargetHdg);
    } else if (strcmp(path, "steering.rudderAngle") == 0) {
        num(out.rudder, FI::Rudder);
    } else if (strcmp(path, "environment.current.setTrue") == 0 ||
               strcmp(path, "environment.current.drift.setTrue") == 0) {
        num(out.currentSetTrue, FI::CurrentSet);
    } else if (strcmp(path, "environment.current.drift") == 0 ||
               strcmp(path, "environment.current.speed") == 0) {
        num(out.currentDrift, FI::CurrentDrift);
    } else if (strcmp(path, "steering.autopilot.state") == 0) {
        const char *s = val.as<const char *>();
        if (s) {
            strncpy(out.apState, s, sizeof(out.apState) - 1);
            out.apState[sizeof(out.apState) - 1] = 0;
            if (touched) *touched |= boat::field_bit(FI::ApState);
        }
    }
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn, boat::FieldMask *touched);

int applyDelta(const char *json, size_t len, boat::View &out, ArduinoJson::Allocator *alloc,
               PathStore *dyn, boat::FieldMask *touched) {
    // alloc==nullptr -> default (internal heap) allocator. The device
    // build passes &yeyboats::psram_json so 1+ Hz SK deltas don't churn
    // the tiny internal heap (largest free block was ~7 KB at idle).
    // Host tests pass nullptr and use the default.
    if (alloc) {
        JsonDocument doc(alloc);
        return apply_delta_impl(json, len, out, doc, dyn, touched);
    }
    JsonDocument doc;
    return apply_delta_impl(json, len, out, doc, dyn, touched);
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn, boat::FieldMask *touched) {
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
            // Resolve the value variant once (each operator[] is a member
            // scan of the object - this is the parse hot path).
            JsonVariant val = v["value"];
            applyValue(p, val, out, touched);
            // Mirror numeric deltas into the dynamic store so authored fields
            // can render arbitrary paths by string (typed boat::View still drives
            // the built-in screens).
            if (dyn && isNumeric(val)) dyn->set(p, val.as<double>());
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
