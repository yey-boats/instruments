// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "web_pw_store.h"

#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/version.h>
#include <string.h>

#include "net.h"
#include "storage.h"
#include "web_pw.h"

namespace web_pw_store {

namespace {

constexpr const char *NS = "web";
constexpr const char *KEY_PLAIN = "pass";  // legacy (<= 0.5), migrated away
constexpr const char *KEY_RECORD = "pass_h";

// -1 unknown, 0 no password, 1 password provisioned.
volatile int s_set = -1;

bool pbkdf2_sha256(const char *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[web_pw::DK_LEN]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03030000
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, pw_len, salt,
                                         salt_len, iterations, web_pw::DK_LEN, out) == 0;
#else
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, info, 1 /* HMAC */) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)pw, pw_len, salt, salt_len,
                                        iterations, web_pw::DK_LEN, out) == 0;
    mbedtls_md_free(&ctx);
    return ok;
#endif
}

bool write_record(storage::Namespace &p, const char *pw) {
    uint8_t salt[web_pw::SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    char rec[web_pw::RECORD_MAX];
    if (!web_pw::make_record(pw, salt, web_pw::ITERATIONS, pbkdf2_sha256, rec, sizeof(rec)))
        return false;
    if (!p.put_string(KEY_RECORD, rec)) return false;
    p.remove(KEY_PLAIN);
    return true;
}

}  // namespace

void migrate() {
    storage::Namespace p(NS, false);
    std::string plain = p.get_string(KEY_PLAIN, "");
    std::string rec = p.get_string(KEY_RECORD, "");
    switch (web_pw::migration(plain.c_str(), rec.c_str())) {
    case web_pw::Migration::HashPlaintext:
        if (write_record(p, plain.c_str())) {
            net::logf("[web] migrated plaintext web password to a salted hash");
        } else {
            net::logf("[web] web password migration FAILED; keeping legacy key");
        }
        break;
    case web_pw::Migration::DropPlaintext:
        p.remove(KEY_PLAIN);
        net::logf("[web] dropped stale plaintext web password");
        break;
    case web_pw::Migration::None:
        break;
    }
    // Wipe the local copy of the plaintext.
    if (!plain.empty()) memset(&plain[0], 0, plain.size());
    s_set = -1;  // recomputed from NVS on the next is_set()
}

bool set_password(const char *pw) {
    if (!pw || !pw[0]) return false;
    // The manager re-sends webAuth.password on config applies; skip the NVS
    // write (and the new salt) when the stored record already matches.
    if (verify(pw, strlen(pw))) {
        storage::Namespace r(NS, true);
        if (web_pw::is_record(r.get_string(KEY_RECORD, "").c_str())) return true;
    }
    storage::Namespace p(NS, false);
    bool ok = write_record(p, pw);
    if (ok) s_set = 1;
    return ok;
}

// Fail safe: if a migration could not write the record (NVS full/error), the
// legacy plaintext stays and still counts as "set" and still verifies, so a
// failed migration never opens the API.
bool is_set() {
    if (s_set < 0) {
        storage::Namespace p(NS, true);
        bool set = web_pw::is_record(p.get_string(KEY_RECORD, "").c_str()) ||
                   !p.get_string(KEY_PLAIN, "").empty();
        s_set = set ? 1 : 0;
    }
    return s_set == 1;
}

bool verify(const char *pw, size_t len) {
    if (!pw || len == 0) return false;
    std::string rec, plain;
    {
        storage::Namespace p(NS, true);
        rec = p.get_string(KEY_RECORD, "");
        if (!web_pw::is_record(rec.c_str())) plain = p.get_string(KEY_PLAIN, "");
    }
    if (web_pw::is_record(rec.c_str())) return web_pw::verify(rec.c_str(), pw, len, pbkdf2_sha256);
    if (plain.empty()) return false;
    bool ok = web_pw::ct_equal(plain.data(), plain.size(), pw, len);
    memset(&plain[0], 0, plain.size());
    return ok;
}

}  // namespace web_pw_store
