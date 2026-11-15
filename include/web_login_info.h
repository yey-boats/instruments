#pragma once

// The Settings screen's "web login" row: how to reach and sign in to this
// display's own web UI and its MIDL editor (/editor).
//
// The device cannot show a stored password: since instruments#30 it keeps
// only a salted PBKDF2 record in NVS (web_pw.h), never the plaintext. A
// managed display gets its password from the Display Manager (webAuth push),
// which keeps the plaintext and offers an owner-only "Show password" on the
// device's page. A display that never had a manager has no password and its
// web UI is open (web_auth_policy.h "setup-open") until the owner makes one
// on the display ("new pass", web_pw_new.h), which is shown once. So the row
// says who to sign in as, where, and where the password lives.
//
// Pure C++ (host-tested in test/test_web_login_info).

#include <stddef.h>

#include "web_auth_policy.h"

namespace web_login_info {

struct Input {
    web_auth_policy::State state;
    const char *username;  // NVS web/user (or DEFAULT_USER)
    const char *address;   // current IP, "" / "0.0.0.0" while unknown
    bool manager_paired;   // saved manager endpoint + device token
};

// False for null, "" and "0.0.0.0" (no WiFi / no lease yet).
bool address_known(const char *address);

// `who`: the sign-in line (username + where the password is, or why there is
// no login). `where`: "http://<address>/editor", or a hint while the address
// is unknown. Both always NUL-terminated (truncated to fit).
void describe(const Input &in, char *who, size_t who_n, char *where, size_t where_n);

}  // namespace web_login_info
