// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "web_login_info.h"

#include <stdio.h>
#include <string.h>

namespace web_login_info {

bool address_known(const char *address) {
    return address && address[0] && strcmp(address, "0.0.0.0") != 0;
}

void describe(const Input &in, char *who, size_t who_n, char *where, size_t where_n) {
    if (who && who_n) {
        const char *user =
            in.username && in.username[0] ? in.username : web_auth_policy::DEFAULT_USER;
        switch (in.state) {
        case web_auth_policy::State::Enforced:
            snprintf(who, who_n, "user %.31s, password: %s", user,
                     in.manager_paired ? "Display Manager device page"
                                       : "kept by the Display Manager");
            break;
        case web_auth_policy::State::DisabledByOwner:
            snprintf(who, who_n, "login off (web auth disabled)");
            break;
        case web_auth_policy::State::SetupOpen:
        default:
            snprintf(who, who_n, "open: no password set");
            break;
        }
    }
    if (where && where_n) {
        if (address_known(in.address))
            snprintf(where, where_n, "http://%s/editor", in.address);
        else
            snprintf(where, where_n, "no address yet (WiFi not connected)");
    }
}

}  // namespace web_login_info
