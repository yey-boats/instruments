#pragma once

// instruments#21: when the device web API enforces HTTP Basic Auth.
//
// Credential story (no built-in shared password):
//   - A factory-fresh device has NO web password. In that state the API is
//     open so first-boot provisioning (setup AP captive page, BLE, touch
//     WiFi screen) keeps working - this is "setup-open".
//   - The device is "set up" for web auth once a password is provisioned in
//     NVS web/pass (today the SignalK manager pushes webAuth.{username,
//     password} on pairing). From then on Basic Auth is enforced BY DEFAULT:
//     the NVS web/auth flag defaults to on when absent.
//   - Only an explicit owner opt-out (webAuth.enabled=false -> web/auth=0)
//     turns it off again.
//
// Pure C++ so the rule is host-tested (test/test_web_auth_policy).

#include <stdint.h>

namespace web_auth_policy {

// Value used for NVS "web"/"auth" when the key is absent.
constexpr uint8_t FLAG_DEFAULT = 1;
// Basic Auth username when NVS "web"/"user" is absent.
constexpr const char *DEFAULT_USER = "espdisp";

enum class State : uint8_t {
    SetupOpen,        // no password provisioned yet: open for provisioning
    Enforced,         // password set and not opted out: Basic Auth required
    DisabledByOwner,  // password may be set, but webAuth.enabled=false
};

State state(uint8_t flag, const char *password);
bool enforced(uint8_t flag, const char *password);
// "setup-open" | "basic" | "disabled"
const char *state_name(State s);

}  // namespace web_auth_policy
