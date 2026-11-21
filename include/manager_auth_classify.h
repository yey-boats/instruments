#pragma once

// Pure-C++ classifier for a manager heartbeat's HTTP 401/403 response body.
//
// instruments#91: one 401 in front of the manager plugin wiped a valid,
// just-delivered device token, even when the 401 actually came from
// SignalK's own security layer (missing/expired sk_token) rather than the
// plugin rejecting the device token itself. Both cases arrive at the
// firmware as a bare HTTP 401/403 - the only way to tell them apart is the
// shape of the response body:
//
//  - SignalK security rejects the request before it ever reaches the
//    plugin: the body is SignalK's HTML login page (or anything else that
//    isn't the plugin's own error JSON). The device token may still be
//    perfectly valid.
//  - The plugin itself decided the *device* token is bad: the body is JSON
//    shaped exactly like {"error":{"code":"unauthorized", ...}}.
//
// Only the second case should ever cause the firmware to drop its saved
// device token. Extracted out of manager.cpp so the decision is
// host-testable without an HTTPClient/WiFi dependency.

#include <stdint.h>

#include <string>

namespace manager_auth_classify {

enum class HeartbeatAuthFailure : uint8_t {
    // The manager plugin's own response body says the device token is
    // invalid. Safe to clear the saved token and fall back to
    // provisioning.
    ManagerRejectedToken,
    // Everything else: an empty/oversized body, SignalK's HTML login
    // page, or JSON that isn't the plugin's specific unauthorized shape.
    // The saved device token is presumed still good - keep it and
    // re-register with it rather than wiping it.
    Other,
};

// Classify the body of an HTTP 401/403 response to a heartbeat POST.
// `body` may be empty (e.g. when the response exceeded the read cap and
// was never fetched).
HeartbeatAuthFailure classify_heartbeat_401(const std::string &body);

// Human-readable reason for the log line, matching the two cases above -
// the wording the issue asked for.
const char *reason_for(HeartbeatAuthFailure f);

}  // namespace manager_auth_classify
