#include "web_auth_policy.h"

namespace web_auth_policy {

State state(uint8_t flag, const char *password) {
    if (flag == 0) return State::DisabledByOwner;
    if (password == nullptr || password[0] == '\0') return State::SetupOpen;
    return State::Enforced;
}

bool enforced(uint8_t flag, const char *password) {
    return state(flag, password) == State::Enforced;
}

const char *state_name(State s) {
    switch (s) {
    case State::Enforced:
        return "basic";
    case State::DisabledByOwner:
        return "disabled";
    case State::SetupOpen:
    default:
        return "setup-open";
    }
}

}  // namespace web_auth_policy
