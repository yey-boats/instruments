// Controller-side IP binding for the espdisp control protocol. Serializes the
// generated request structs with ArduinoJson (to_json), POSTs/GETs them against
// the target's /api/p2p/* endpoints, and parses the acks back via from_json.
//
// These calls run on the harness's own loop (not a UI task), so blocking
// HTTPClient I/O is acceptable here.
#include "proto_ip.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

namespace proto_ip {

namespace {

// GET `base + path`, on a 2xx deserialize the body and from_json() into `out`.
// Returns true on a 2xx response with a parseable body.
template <typename T> bool http_get(const String &base, const char *path, T &out) {
    HTTPClient http;
    if (!http.begin(base + path)) return false;
    http.setTimeout(3000);
    int code = http.GET();
    bool ok = false;
    if (code / 100 == 2) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            from_json(doc.as<JsonObjectConst>(), out);
            ok = true;
        }
    }
    http.end();
    return ok;
}

// to_json(req) -> POST `base + path`; on a 2xx deserialize the ack body and
// from_json() into `ack`. Returns true on a 2xx with a parseable ack.
template <typename Req, typename Ack>
bool http_post(const String &base, const char *path, const Req &req, Ack &ack) {
    JsonDocument doc;
    to_json(doc.to<JsonObject>(), req);
    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    if (!http.begin(base + path)) return false;
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    bool ok = false;
    if (code / 100 == 2) {
        JsonDocument resp;
        if (deserializeJson(resp, http.getString()) == DeserializationError::Ok) {
            from_json(resp.as<JsonObjectConst>(), ack);
            ok = true;
        }
    }
    http.end();
    return ok;
}

// to_json(req) -> POST `base + path`; ignore the body, return true on 2xx.
template <typename Req>
bool http_post_no_ack(const String &base, const char *path, const Req &req) {
    JsonDocument doc;
    to_json(doc.to<JsonObject>(), req);
    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    if (!http.begin(base + path)) return false;
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    http.end();
    return code / 100 == 2;
}

}  // namespace

bool get_device(const String &base, proto::DeviceRecord &out) {
    return http_get(base, "/api/p2p/device", out);
}

bool get_state(const String &base, proto::ControlState &out) {
    return http_get(base, "/api/p2p/state", out);
}

bool attach(const String &base, const proto::Attach &a, proto::AttachAck &ack) {
    return http_post(base, "/api/p2p/attach", a, ack);
}

bool do_switch(const String &base, const proto::Switch &s, proto::SwitchAck &ack) {
    return http_post(base, "/api/p2p/switch", s, ack);
}

bool heartbeat(const String &base, const char *sessionId) {
    proto::Heartbeat hb{};
    strncpy(hb.v, "1.0", sizeof(hb.v) - 1);
    strncpy(hb.sessionId, sessionId ? sessionId : "", sizeof(hb.sessionId) - 1);
    return http_post_no_ack(base, "/api/p2p/heartbeat", hb);
}

bool detach(const String &base, const char *sessionId) {
    proto::Detach d{};
    strncpy(d.v, "1.0", sizeof(d.v) - 1);
    strncpy(d.sessionId, sessionId ? sessionId : "", sizeof(d.sessionId) - 1);
    return http_post_no_ack(base, "/api/p2p/detach", d);
}

}  // namespace proto_ip
