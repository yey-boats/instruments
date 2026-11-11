#pragma once

// instruments#30: device-side storage for the web password (NVS "web").
// Wraps the pure web_pw rules (web_pw.h) with NVS, a random salt
// (esp_fill_random), mbedtls PBKDF2-HMAC-SHA256 and the RAM verify cache.

#include <stddef.h>

#include <string>

namespace web_pw_store {

// Boot (web::setup): hash a legacy plaintext "web"/"pass" into "pass_h".
// The plaintext is erased here only if the running image is already
// confirmed; while it is PENDING_VERIFY it stays for a possible rollback.
void migrate();

// Call once the running image is marked valid (manager::setup, right after
// esp_ota_mark_app_valid_cancel_rollback): erases the kept plaintext.
void image_confirmed();

// Store a new password as a fresh salted record (and, while the image is
// still pending, as plaintext for the previous image too).
bool set_password(const char *pw);

// True once a password is provisioned (cached in RAM; cheap to call from any
// task, including the NimBLE host task).
bool is_set();

// Verify a presented password: RAM cache hit (constant-time tag compare,
// CACHE_TTL_MS) or a full PBKDF2 derive + constant-time compare.
bool verify(const char *pw, size_t len);

}  // namespace web_pw_store
