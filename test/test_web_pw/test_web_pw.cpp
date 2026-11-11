// instruments#30: the web password is stored in NVS as a salted PBKDF2 record,
// never as plaintext. Basic Auth verification derives a key from the presented
// password with the stored salt/iterations and compares in constant time.
// The KDF is injected (mbedtls PBKDF2-HMAC-SHA256 on the device); these tests
// drive the record format, verification and migration rules with a
// deterministic stand-in KDF.

#include <string.h>
#include <unity.h>

#include <map>
#include <string>

#include "web_pw.h"

void setUp(void) {
}
void tearDown(void) {
}

static int g_kdf_calls = 0;

// Deterministic stand-in: depends on every input byte, the salt and the
// iteration count (not cryptographic; the device uses PBKDF2-HMAC-SHA256).
static bool fake_kdf(const char *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t out[web_pw::DK_LEN]) {
    ++g_kdf_calls;
    uint32_t h = 2166136261u ^ iterations;
    for (size_t i = 0; i < web_pw::DK_LEN; ++i) {
        for (size_t j = 0; j < pw_len; ++j)
            h = (h ^ (uint8_t)pw[j]) * 16777619u;
        for (size_t j = 0; j < salt_len; ++j)
            h = (h ^ salt[j]) * 16777619u;
        h = (h ^ (uint32_t)i) * 16777619u;
        out[i] = (uint8_t)(h >> 24);
    }
    return true;
}

static const uint8_t SALT_A[web_pw::SALT_LEN] = {1, 2,  3,  4,  5,  6,  7,  8,
                                                 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t SALT_B[web_pw::SALT_LEN] = {16, 15, 14, 13, 12, 11, 10, 9,
                                                 8,  7,  6,  5,  4,  3,  2,  1};

static void test_record_is_not_plaintext_and_verifies() {
    char rec[web_pw::RECORD_MAX];
    TEST_ASSERT_TRUE(web_pw::make_record("s3cret", SALT_A, 1000, fake_kdf, rec, sizeof(rec)));
    TEST_ASSERT_NULL(strstr(rec, "s3cret"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(rec, "pbkdf2-sha256$1000$", 19));
    TEST_ASSERT_TRUE(web_pw::is_record(rec));
    TEST_ASSERT_TRUE(web_pw::verify(rec, "s3cret", 6, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(rec, "s3cre", 5, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(rec, "s3cret!", 7, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(rec, "", 0, fake_kdf));
}

static void test_salt_changes_record() {
    char a[web_pw::RECORD_MAX], b[web_pw::RECORD_MAX];
    TEST_ASSERT_TRUE(web_pw::make_record("same", SALT_A, 1000, fake_kdf, a, sizeof(a)));
    TEST_ASSERT_TRUE(web_pw::make_record("same", SALT_B, 1000, fake_kdf, b, sizeof(b)));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    TEST_ASSERT_TRUE(web_pw::verify(a, "same", 4, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::verify(b, "same", 4, fake_kdf));
}

static void test_iterations_from_record_are_used() {
    char rec[web_pw::RECORD_MAX];
    TEST_ASSERT_TRUE(web_pw::make_record("pw", SALT_A, 7, fake_kdf, rec, sizeof(rec)));
    TEST_ASSERT_EQUAL_INT(0, strncmp(rec, "pbkdf2-sha256$7$", 16));
    TEST_ASSERT_TRUE(web_pw::verify(rec, "pw", 2, fake_kdf));
}

static void test_malformed_records_never_verify() {
    const char *bad[] = {
        "",
        "s3cret",  // legacy plaintext is not a record
        "pbkdf2-sha256$",
        "pbkdf2-sha256$1000$0102$00",
        "pbkdf2-sha256$0$0102030405060708090a0b0c0d0e0f10$"  // zero iterations
        "0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$1000$zz02030405060708090a0b0c0d0e0f10$"
        "0000000000000000000000000000000000000000000000000000000000000000",
        "md5$1000$0102030405060708090a0b0c0d0e0f10$"
        "0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$99999999$0102030405060708090a0b0c0d0e0f10$"
        "0000000000000000000000000000000000000000000000000000000000000000",
    };
    for (const char *b : bad) {
        g_kdf_calls = 0;
        TEST_ASSERT_FALSE_MESSAGE(web_pw::is_record(b), b);
        TEST_ASSERT_FALSE_MESSAGE(web_pw::verify(b, "s3cret", 6, fake_kdf), b);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_kdf_calls, b);
    }
    TEST_ASSERT_FALSE(web_pw::verify(nullptr, "x", 1, fake_kdf));
}

static void test_make_record_bounds() {
    char small[16];
    TEST_ASSERT_FALSE(web_pw::make_record("pw", SALT_A, 1000, fake_kdf, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    char rec[web_pw::RECORD_MAX];
    TEST_ASSERT_FALSE(web_pw::make_record("", SALT_A, 1000, fake_kdf, rec, sizeof(rec)));
    TEST_ASSERT_FALSE(web_pw::make_record(nullptr, SALT_A, 1000, fake_kdf, rec, sizeof(rec)));
    TEST_ASSERT_FALSE(web_pw::make_record("pw", SALT_A, 0, fake_kdf, rec, sizeof(rec)));
}

static void test_ct_equal() {
    TEST_ASSERT_TRUE(web_pw::ct_equal("abc", 3, "abc", 3));
    TEST_ASSERT_FALSE(web_pw::ct_equal("abc", 3, "abd", 3));
    TEST_ASSERT_FALSE(web_pw::ct_equal("abc", 3, "ab", 2));
    TEST_ASSERT_TRUE(web_pw::ct_equal("", 0, "", 0));
}

// A decoded Basic credential "user:pass" splits at the FIRST colon (the
// password may contain colons; RFC 7617 forbids one in the user-id).
static void test_split_basic() {
    const char *user;
    size_t ulen;
    const char *pass;
    size_t plen;
    const char *d = "espdisp:pa:ss";
    TEST_ASSERT_TRUE(web_pw::split_basic(d, strlen(d), &user, &ulen, &pass, &plen));
    TEST_ASSERT_EQUAL_INT(7, (int)ulen);
    TEST_ASSERT_EQUAL_INT(0, strncmp(user, "espdisp", 7));
    TEST_ASSERT_EQUAL_INT(5, (int)plen);
    TEST_ASSERT_EQUAL_INT(0, strncmp(pass, "pa:ss", 5));
    TEST_ASSERT_FALSE(web_pw::split_basic("nocolon", 7, &user, &ulen, &pass, &plen));
    TEST_ASSERT_TRUE(web_pw::split_basic("u:", 2, &user, &ulen, &pass, &plen));
    TEST_ASSERT_EQUAL_INT(0, (int)plen);
}

// ---- NVS state machine (rollback-safe migration, PR #34 review) ----------

struct MapKv : web_pw::Kv {
    std::map<std::string, std::string> m;
    bool fail_puts = false;
    std::string get(const char *k) override {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    }
    bool put(const char *k, const char *v) override {
        if (fail_puts) return false;
        m[k] = v;
        return true;
    }
    void remove(const char *k) override { m.erase(k); }
    bool has(const char *k) const { return m.count(k) != 0; }
};

// What firmware older than #30 does with the same NVS: plaintext only.
static bool old_firmware_accepts(MapKv &kv, const char *pw) {
    std::string p = kv.get(web_pw::KEY_PLAIN);
    return !p.empty() && p == pw;
}

static void test_reconcile_pending_keeps_plaintext_for_rollback() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    // First boot of the new image, still PENDING_VERIFY.
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, true, SALT_A, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::is_record(kv.get(web_pw::KEY_RECORD).c_str()));
    TEST_ASSERT_TRUE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "legacy", 6, fake_kdf));
    // Bootloader rolls back: the old firmware still has its password.
    TEST_ASSERT_TRUE(old_firmware_accepts(kv, "legacy"));
}

static void test_reconcile_after_confirm_erases_plaintext() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, true, SALT_A, fake_kdf));
    std::string rec = kv.get(web_pw::KEY_RECORD);
    // manager::setup marked the image valid -> confirm.
    g_kdf_calls = 0;
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, false, SALT_B, fake_kdf));
    TEST_ASSERT_FALSE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_EQUAL_STRING(rec.c_str(), kv.get(web_pw::KEY_RECORD).c_str());  // not re-salted
    TEST_ASSERT_TRUE(web_pw::verify(kv, "legacy", 6, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::is_set(kv));
}

static void test_reconcile_confirmed_boot_hashes_and_erases() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, false, SALT_A, fake_kdf));
    TEST_ASSERT_FALSE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "legacy", 6, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(kv, "other", 5, fake_kdf));
}

// Downgrade, the manager re-pushes a NEW password into the plaintext key,
// then upgrade again: the plaintext is newer than the stale record.
static void test_reconcile_rehashes_newer_plaintext() {
    MapKv kv;
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "first", false, SALT_A, fake_kdf));
    kv.m[web_pw::KEY_PLAIN] = "second";
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, false, SALT_B, fake_kdf));
    TEST_ASSERT_FALSE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "second", 6, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(kv, "first", 5, fake_kdf));
}

static void test_reconcile_write_failure_keeps_plaintext() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    kv.fail_puts = true;
    TEST_ASSERT_FALSE(web_pw::reconcile(kv, false, SALT_A, fake_kdf));
    TEST_ASSERT_TRUE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_TRUE(web_pw::is_set(kv));  // fails closed
    TEST_ASSERT_TRUE(web_pw::verify(kv, "legacy", 6, fake_kdf));
}

static void test_reconcile_nothing_to_do() {
    MapKv kv;
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, true, SALT_A, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, false, SALT_A, fake_kdf));
    TEST_ASSERT_TRUE(kv.m.empty());
    TEST_ASSERT_FALSE(web_pw::is_set(kv));
    TEST_ASSERT_FALSE(web_pw::verify(kv, "x", 1, fake_kdf));
}

static void test_set_password_pending_writes_both() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, true, SALT_A, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "fresh", true, SALT_B, fake_kdf));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "fresh", 5, fake_kdf));
    TEST_ASSERT_TRUE(old_firmware_accepts(kv, "fresh"));
    TEST_ASSERT_TRUE(web_pw::reconcile(kv, false, SALT_A, fake_kdf));
    TEST_ASSERT_FALSE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "fresh", 5, fake_kdf));
}

static void test_set_password_confirmed_record_only() {
    MapKv kv;
    kv.m[web_pw::KEY_PLAIN] = "legacy";
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "fresh", false, SALT_A, fake_kdf));
    TEST_ASSERT_FALSE(kv.has(web_pw::KEY_PLAIN));
    TEST_ASSERT_NULL(strstr(kv.get(web_pw::KEY_RECORD).c_str(), "fresh"));
    TEST_ASSERT_TRUE(web_pw::verify(kv, "fresh", 5, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify(kv, "legacy", 6, fake_kdf));
    // Re-sent unchanged: no new salt / NVS write.
    std::string rec = kv.get(web_pw::KEY_RECORD);
    kv.fail_puts = true;
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "fresh", false, SALT_B, fake_kdf));
    TEST_ASSERT_EQUAL_STRING(rec.c_str(), kv.get(web_pw::KEY_RECORD).c_str());
    TEST_ASSERT_FALSE(web_pw::set_password(kv, "", false, SALT_B, fake_kdf));
}

// ---- verify cache --------------------------------------------------------

static int g_tag_calls = 0;
static bool fake_tag(const char *rec, const char *pw, size_t pw_len, uint8_t out[web_pw::TAG_LEN]) {
    ++g_tag_calls;
    uint32_t h = 2166136261u;
    for (const char *c = rec; *c; ++c)
        h = (h ^ (uint8_t)*c) * 16777619u;
    h = (h ^ 0xff) * 16777619u;
    for (size_t i = 0; i < web_pw::TAG_LEN; ++i) {
        for (size_t j = 0; j < pw_len; ++j)
            h = (h ^ (uint8_t)pw[j]) * 16777619u;
        out[i] = (uint8_t)(h >> 24);
    }
    return true;
}

static void test_cache_skips_kdf_within_ttl() {
    MapKv kv;
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "pw", false, SALT_A, fake_kdf));
    web_pw::VerifyCache c;
    g_kdf_calls = 0;
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 1000));
    TEST_ASSERT_EQUAL_INT(1, g_kdf_calls);
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 1000 + 5000));
    TEST_ASSERT_TRUE(
        web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 1000 + web_pw::CACHE_TTL_MS - 1));
    TEST_ASSERT_EQUAL_INT(1, g_kdf_calls);
    // Expired: full verify again, which refreshes the entry.
    TEST_ASSERT_TRUE(
        web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 1000 + web_pw::CACHE_TTL_MS));
    TEST_ASSERT_EQUAL_INT(2, g_kdf_calls);
}

static void test_cache_wrong_password_never_hits_or_caches() {
    MapKv kv;
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "pw", false, SALT_A, fake_kdf));
    web_pw::VerifyCache c;
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 0));
    g_kdf_calls = 0;
    TEST_ASSERT_FALSE(web_pw::verify_cached(kv, "px", 2, fake_kdf, fake_tag, c, 10));
    TEST_ASSERT_FALSE(web_pw::verify_cached(kv, "px", 2, fake_kdf, fake_tag, c, 20));
    TEST_ASSERT_EQUAL_INT(2, g_kdf_calls);  // every miss pays the KDF
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "pw", 2, fake_kdf, fake_tag, c, 30));
    TEST_ASSERT_EQUAL_INT(2, g_kdf_calls);  // the good entry survived
}

static void test_cache_invalidated_by_password_change() {
    MapKv kv;
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "old", false, SALT_A, fake_kdf));
    web_pw::VerifyCache c;
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "old", 3, fake_kdf, fake_tag, c, 0));
    TEST_ASSERT_TRUE(web_pw::set_password(kv, "new", false, SALT_B, fake_kdf));
    TEST_ASSERT_FALSE(web_pw::verify_cached(kv, "old", 3, fake_kdf, fake_tag, c, 10));
    TEST_ASSERT_TRUE(web_pw::verify_cached(kv, "new", 3, fake_kdf, fake_tag, c, 20));
}

static void test_cache_ttl_survives_millis_wrap() {
    uint8_t t[web_pw::TAG_LEN] = {7};
    web_pw::VerifyCache c;
    TEST_ASSERT_FALSE(c.hit(t, 0));
    c.store(t, 0xFFFFFF00u);
    TEST_ASSERT_TRUE(c.hit(t, 0x00000100u));
    uint8_t u[web_pw::TAG_LEN] = {8};
    TEST_ASSERT_FALSE(c.hit(u, 0x00000100u));
    c.clear();
    TEST_ASSERT_FALSE(c.hit(t, 0x00000100u));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_record_is_not_plaintext_and_verifies);
    RUN_TEST(test_salt_changes_record);
    RUN_TEST(test_iterations_from_record_are_used);
    RUN_TEST(test_malformed_records_never_verify);
    RUN_TEST(test_make_record_bounds);
    RUN_TEST(test_ct_equal);
    RUN_TEST(test_split_basic);
    RUN_TEST(test_reconcile_pending_keeps_plaintext_for_rollback);
    RUN_TEST(test_reconcile_after_confirm_erases_plaintext);
    RUN_TEST(test_reconcile_confirmed_boot_hashes_and_erases);
    RUN_TEST(test_reconcile_rehashes_newer_plaintext);
    RUN_TEST(test_reconcile_write_failure_keeps_plaintext);
    RUN_TEST(test_reconcile_nothing_to_do);
    RUN_TEST(test_set_password_pending_writes_both);
    RUN_TEST(test_set_password_confirmed_record_only);
    RUN_TEST(test_cache_skips_kdf_within_ttl);
    RUN_TEST(test_cache_wrong_password_never_hits_or_caches);
    RUN_TEST(test_cache_invalidated_by_password_change);
    RUN_TEST(test_cache_ttl_survives_millis_wrap);
    return UNITY_END();
}
