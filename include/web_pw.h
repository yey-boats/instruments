#pragma once

// instruments#30: the device web password is stored in NVS as a salted
// PBKDF2-HMAC-SHA256 record (NVS "web"/"pass_h"), never as plaintext.
//
//   record = "pbkdf2-sha256$<iterations>$<salt: 32 hex>$<key: 64 hex>"
//
// Basic Auth verification derives a key from the presented password with the
// record's salt and iterations and compares it in constant time. The legacy
// plaintext key ("web"/"pass", firmware <= 0.5) is migrated on boot.
//
// Pure C++: the KDF is injected (mbedtls PBKDF2 on the device, see
// web_pw_store.cpp), so the format/verify/migration rules are host-tested in
// test/test_web_pw.

#include <stddef.h>
#include <stdint.h>

namespace web_pw {

constexpr size_t SALT_LEN = 16;
constexpr size_t DK_LEN = 32;
// Cost per verify on an ESP32-S3 (HW SHA) is a few ms; every authed API call
// pays it once.
constexpr uint32_t ITERATIONS = 1000;
constexpr uint32_t ITERATIONS_MAX = 100000;
// "pbkdf2-sha256$" + 6 digits + "$" + 32 + "$" + 64 + NUL, rounded up.
constexpr size_t RECORD_MAX = 128;

using Kdf = bool (*)(const char *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t out[DK_LEN]);

// Build a record for a non-empty password. Returns false (out = "") on bad
// input or if out is too small.
bool make_record(const char *pw, const uint8_t salt[SALT_LEN], uint32_t iterations, Kdf kdf,
                 char *out, size_t n);

// True if `s` is a well-formed record (never true for legacy plaintext).
bool is_record(const char *s);

// Verify a presented password (pw, pw_len; may contain NULs-free bytes of a
// Basic credential) against a record. A malformed record never verifies and
// never calls the KDF.
bool verify(const char *record, const char *pw, size_t pw_len, Kdf kdf);

// Constant-time equality (length mismatch -> false, still scans `a`).
bool ct_equal(const void *a, size_t a_len, const void *b, size_t b_len);

// Split a decoded Basic credential at the FIRST ':' into user and password.
bool split_basic(const char *decoded, size_t len, const char **user, size_t *user_len,
                 const char **pass, size_t *pass_len);

// What boot migration should do with the NVS keys.
enum class Migration {
    None,           // nothing to do
    HashPlaintext,  // legacy plaintext present, no valid record: hash it, drop it
    DropPlaintext,  // a valid record already exists: drop the stale plaintext
};
Migration migration(const char *plaintext, const char *record);

}  // namespace web_pw
