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
        } else if (endsWith(path, ".runTime")) {
            num(out.engineHours, FI::EngineHours);  // seconds (SI); display shows hours
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

// Context helpers (phase 5: AIS + notifications routing) -------------------

// True when a delta context names OUR vessel. Absent context and the literal
// "vessels.self" are always self; otherwise compare against the hello-derived
// self id, tolerating the "vessels." prefix on either side (servers report
// hello self as "vessels.urn:..." but some emit bare "urn:..." contexts).
static bool context_is_self(const char *ctx, const char *self_id) {
    if (!ctx || !ctx[0]) return true;
    if (strcmp(ctx, "vessels.self") == 0) return true;
    if (!self_id || !self_id[0]) return false;
    const char *a = ctx;
    const char *b = self_id;
    if (strncmp(a, "vessels.", 8) == 0) a += 8;
    if (strncmp(b, "vessels.", 8) == 0) b += 8;
    return strcmp(a, b) == 0;
}

// MMSI from "vessels.urn:mrn:imo:mmsi:244813000". 0 when the context is not
// an mmsi urn (e.g. uuid vessels, shore stations) - those targets are dropped
// (the AIS store is keyed by MMSI).
static uint32_t context_mmsi(const char *ctx) {
    if (!ctx) return 0;
    const char *m = strstr(ctx, "mmsi:");
    if (!m) return 0;
    m += 5;
    uint32_t v = 0;
    int digits = 0;
    for (; *m; ++m) {
        if (*m < '0' || *m > '9') return 0;
        v = v * 10u + (uint32_t)(*m - '0');
        if (++digits > 9) return 0;  // MMSI is at most 9 digits
    }
    return digits > 0 ? v : 0;
}

// notifications.<suffix> value object {state, message, method[]} -> store
// upsert. Null value or state "normal" clears. Returns true when the pair
// was routed (counts as value-bearing for link liveness).
static bool apply_notification(const char *path, JsonVariant val, const DeltaSinks *sinks) {
    static constexpr size_t PREFIX = 14;  // strlen("notifications.")
    const char *suffix = path + PREFIX;
    if (!suffix[0] || !sinks || !sinks->notifications) return false;
    if (val.isNull()) {
        // Nulled-out notification value = alarm gone.
        sinks->notifications->upsert(suffix, notif::State::Normal, nullptr, 0, sinks->now_ms);
        return true;
    }
    if (!val.is<JsonObject>()) return false;
    const char *state = val["state"];
    const char *message = val["message"];
    uint8_t method = 0;
    JsonArray m = val["method"].as<JsonArray>();
    if (!m.isNull()) {
        for (JsonVariant mm : m) {
            const char *s = mm.as<const char *>();
            if (!s) continue;
            if (strcmp(s, "visual") == 0) method |= notif::METHOD_VISUAL;
            if (strcmp(s, "sound") == 0) method |= notif::METHOD_SOUND;
        }
    }
    sinks->notifications->upsert(suffix, notif::state_from_string(state), message, method,
                                 sinks->now_ms);
    return true;
}

// One path/value pair of a NON-SELF vessel delta -> AIS store. Never touches
// boat::View. Returns true when the pair was routed.
static bool apply_ais_value(uint32_t mmsi, const char *path, JsonVariant val,
                            const DeltaSinks *sinks) {
    ais::Store *st = sinks->ais;
    const uint32_t now = sinks->now_ms;
    const ais::VesselClass cls = ais::VesselClass::Unknown;
    if (strcmp(path, "navigation.position") == 0) {
        if (!val.is<JsonObject>()) return false;
        double la = asDouble(val["latitude"]);
        double lo = asDouble(val["longitude"]);
        if (isnan(la) || isnan(lo)) return false;
        return st->upsert_position(mmsi, la, lo, NAN, NAN, NAN, cls, now) >= 0;
    }
    if (strcmp(path, "navigation.speedOverGround") == 0) {
        if (!isNumeric(val)) return false;
        return st->upsert_position(mmsi, NAN, NAN, (float)val.as<double>(), NAN, NAN, cls, now) >=
               0;
    }
    if (strcmp(path, "navigation.courseOverGroundTrue") == 0) {
        if (!isNumeric(val)) return false;
        return st->upsert_position(mmsi, NAN, NAN, NAN, (float)val.as<double>(), NAN, cls, now) >=
               0;
    }
    if (strcmp(path, "navigation.headingTrue") == 0) {
        if (!isNumeric(val)) return false;
        return st->upsert_position(mmsi, NAN, NAN, NAN, NAN, (float)val.as<double>(), cls, now) >=
               0;
    }
    // Vessel name from the static tree: either the root object (path "")
    // carrying {"name": ...} or the "name" leaf directly.
    if (strcmp(path, "name") == 0) {
        const char *nm = val.as<const char *>();
        if (!nm || !nm[0]) return false;
        return st->upsert_static(mmsi, nm, cls, now) >= 0;
    }
    if (path[0] == 0 && val.is<JsonObject>()) {
        const char *nm = val["name"];
        if (!nm || !nm[0]) return false;
        return st->upsert_static(mmsi, nm, cls, now) >= 0;
    }
    return false;
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn, boat::FieldMask *touched, const DeltaSinks *sinks);

int applyDelta(const char *json, size_t len, boat::View &out, ArduinoJson::Allocator *alloc,
               PathStore *dyn, boat::FieldMask *touched, const DeltaSinks *sinks) {
    // alloc==nullptr -> default (internal heap) allocator. The device
    // build passes &yeyboats::psram_json so 1+ Hz SK deltas don't churn
    // the tiny internal heap (largest free block was ~7 KB at idle).
    // Host tests pass nullptr and use the default.
    if (alloc) {
        JsonDocument doc(alloc);
        return apply_delta_impl(json, len, out, doc, dyn, touched, sinks);
    }
    JsonDocument doc;
    return apply_delta_impl(json, len, out, doc, dyn, touched, sinks);
}

static int apply_delta_impl(const char *json, size_t len, boat::View &out, JsonDocument &doc,
                            PathStore *dyn, boat::FieldMask *touched, const DeltaSinks *sinks) {
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) return -1;
    JsonArray updates = doc["updates"].as<JsonArray>();
    if (updates.isNull()) return 0;
    // Route by context ONCE per delta (context is a top-level field). A
    // non-self vessel's values go to the AIS store and must never leak into
    // the self View/PathStore/touched-mask.
    const char *ctx = doc["context"];
    const bool is_self = context_is_self(ctx, sinks ? sinks->self_id : nullptr);
    const uint32_t mmsi = (!is_self && sinks && sinks->ais) ? context_mmsi(ctx) : 0;
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
            if (!is_self) {
                if (mmsi && apply_ais_value(mmsi, p, val, sinks)) ++count;
                continue;
            }
            if (strncmp(p, "notifications.", 14) == 0) {
                // Alarm path: routed to the notifications store, never a
                // View field (the value is an object, not a metric).
                apply_notification(p, val, sinks);
                ++count;
                continue;
            }
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

static bool parse_hello_impl(JsonDocument &doc, const char *json, size_t len, char *out,
                             size_t cap) {
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    const char *self = doc["self"];
    if (!self || !self[0]) return false;
    strncpy(out, self, cap - 1);
    out[cap - 1] = 0;
    return true;
}

bool parseHello(const char *json, size_t len, char *out, size_t cap,
                ArduinoJson::Allocator *alloc) {
    if (!json || !out || cap == 0) return false;
    if (alloc) {
        JsonDocument doc(alloc);
        return parse_hello_impl(doc, json, len, out, cap);
    }
    JsonDocument doc;
    return parse_hello_impl(doc, json, len, out, cap);
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
