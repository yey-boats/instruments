#pragma once

// instruments#30: the device web password is stored in NVS as a salted
// PBKDF2-HMAC-SHA256 record (NVS "web"/"pass_h"), never as plaintext.
//
//   record = "pbkdf2-sha256$<iterations>$<salt: 32 hex>$<key: 64 hex>"
//
// Basic Auth verification derives a key from the presented password with the
// record's salt and iterations and compares it in constant time. The legacy
// plaintext key ("web"/"pass", older firmware) is migrated on boot.
//
// Rollback safety (PR #34 review): while the running image is still
// PENDING_VERIFY the bootloader may roll back to the previous firmware, which
// only reads the plaintext key. So the plaintext is kept next to the record
// until the image is confirmed (manager::setup marks it valid), and only then
// erased. A password set during that window is written to both keys.
//
// Cost (ESP32-S3, 240 MHz): one PBKDF2-HMAC-SHA256 iteration is two HMAC
// passes = 4 SHA-256 compressions; 1000 iterations = ~4000 compressions plus
// mbedtls per-call overhead, ~20-30 ms with the HW SHA engine (~50 ms in
// software). web_pw_store logs the measured time of the first derive. To keep
// that off every authed request, a successful verify is cached in RAM for
// CACHE_TTL_MS as an HMAC tag (per-boot random key) over record + password;
// a hit is a constant-time tag compare, a password change invalidates it (the
// tag binds the record) and failures are never cached.
//
// Pure C++: the KDF, the tag function and the key-value store are injected
// (mbedtls + NVS on the device, see web_pw_store.cpp), so the format, verify,
// migration and cache rules are host-tested in test/test_web_pw.

#include <stddef.h>
#include <stdint.h>

#include <string>

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

// ---- NVS state machine -------------------------------------------------

constexpr const char *KEY_PLAIN = "pass";     // legacy, read by older firmware
constexpr const char *KEY_RECORD = "pass_h";  // salted record

// Key-value seam over NVS namespace "web" (a map in host tests).
struct Kv {
    virtual ~Kv() = default;
    virtual std::string get(const char *key) = 0;  // "" when missing
    virtual bool put(const char *key, const char *value) = 0;
    virtual void remove(const char *key) = 0;
};

// Boot, and again once the image is confirmed (image_pending = false).
// Legacy plaintext present: (re)hash it unless the record already verifies
// it, then erase the plaintext ONLY if !image_pending. If the record cannot
// be written the plaintext stays (fails closed). Returns false on a write
// failure.
bool reconcile(Kv &kv, bool image_pending, const uint8_t salt[SALT_LEN], Kdf kdf);

// Store a new password. Skips the write when the record already verifies it
// (the manager re-sends it on every config apply). While image_pending the
// plaintext is written too, so a rollback still sees the current password.
bool set_password(Kv &kv, const char *pw, bool image_pending, const uint8_t salt[SALT_LEN],
                  Kdf kdf);

// A password is provisioned (a valid record, or legacy plaintext that could
// not be migrated yet).
bool is_set(Kv &kv);

// Verify against the record (PBKDF2), else the legacy plaintext.
bool verify(Kv &kv, const char *pw, size_t pw_len, Kdf kdf);

// ---- verify cache ------------------------------------------------------

constexpr uint32_t CACHE_TTL_MS = 30000;
constexpr size_t TAG_LEN = 32;

// Keyed tag over (record, password); HMAC-SHA256 with a per-boot random key
// on the device.
using Tagger = bool (*)(const char *record, const char *pw, size_t pw_len, uint8_t out[TAG_LEN]);

struct VerifyCache {
    uint8_t tag[TAG_LEN] = {0};
    uint32_t at_ms = 0;
    bool valid = false;

    bool hit(const uint8_t t[TAG_LEN], uint32_t now_ms) const;  // constant-time compare
    void store(const uint8_t t[TAG_LEN], uint32_t now_ms);
    void clear();
};

// verify() with the cache in front: a hit within the TTL skips the KDF; a
// successful full verify refreshes the entry; a failure leaves it untouched.
bool verify_cached(Kv &kv, const char *pw, size_t pw_len, Kdf kdf, Tagger tagger,
                   VerifyCache &cache, uint32_t now_ms);

}  // namespace web_pw
