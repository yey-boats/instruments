#include "manager_auth_classify.h"

#include <ArduinoJson.h>

namespace manager_auth_classify {

HeartbeatAuthFailure classify_heartbeat_401(const std::string &body) {
    if (body.empty()) return HeartbeatAuthFailure::Other;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err != DeserializationError::Ok) return HeartbeatAuthFailure::Other;

    JsonVariant error_field = doc["error"];
    if (!error_field.is<JsonObject>()) return HeartbeatAuthFailure::Other;

    JsonObject error_obj = error_field.as<JsonObject>();
    const char *code = error_obj["code"] | "";
    if (code && std::string(code) == "unauthorized") {
        return HeartbeatAuthFailure::ManagerRejectedToken;
    }
    return HeartbeatAuthFailure::Other;
}

const char *reason_for(HeartbeatAuthFailure f) {
    switch (f) {
    case HeartbeatAuthFailure::ManagerRejectedToken:
        return "manager rejected the device token";
    case HeartbeatAuthFailure::Other:
        return "SignalK rejected the request: sk_token missing/invalid";
    }
    return "?";
}

}  // namespace manager_auth_classify
