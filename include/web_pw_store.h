#pragma once

// instruments#30: device-side storage for the web password (NVS "web").
// Wraps the pure web_pw record rules with NVS, a random salt
// (esp_fill_random) and mbedtls PBKDF2-HMAC-SHA256. See web_pw.h.

#include <stddef.h>

#include <string>

namespace web_pw_store {

// Boot: hash a legacy plaintext "web"/"pass" into "pass_h" and erase it.
void migrate();

// Store a new password as a fresh salted record; erases any plaintext.
bool set_password(const char *pw);

// True once a password is provisioned (cached in RAM; cheap to call from any
// task, including the NimBLE host task).
bool is_set();

// Verify a presented password (constant-time compare of the derived key).
bool verify(const char *pw, size_t len);

}  // namespace web_pw_store
