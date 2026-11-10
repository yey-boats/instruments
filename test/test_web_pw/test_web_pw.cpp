// instruments#30: the web password is stored in NVS as a salted PBKDF2 record,
// never as plaintext. Basic Auth verification derives a key from the presented
// password with the stored salt/iterations and compares in constant time.
// The KDF is injected (mbedtls PBKDF2-HMAC-SHA256 on the device); these tests
// drive the record format, verification and migration rules with a
// deterministic stand-in KDF.

#include <string.h>
#include <unity.h>

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

// Boot migration of the NVS keys: legacy plaintext "pass" -> "pass_h" record.
static void test_migration_rules() {
    using web_pw::Migration;
    TEST_ASSERT_EQUAL(Migration::None, web_pw::migration("", ""));
    TEST_ASSERT_EQUAL(Migration::None, web_pw::migration(nullptr, nullptr));
    TEST_ASSERT_EQUAL(Migration::HashPlaintext, web_pw::migration("legacy", ""));
    TEST_ASSERT_EQUAL(Migration::HashPlaintext, web_pw::migration("legacy", nullptr));
    // A record already exists: the plaintext is stale, just drop it.
    TEST_ASSERT_EQUAL(Migration::DropPlaintext,
                      web_pw::migration("legacy", "pbkdf2-sha256$1000$"
                                                  "0102030405060708090a0b0c0d0e0f10$"
                                                  "00000000000000000000000000000000"
                                                  "00000000000000000000000000000000"));
    TEST_ASSERT_EQUAL(Migration::None, web_pw::migration("", "pbkdf2-sha256$1000$"
                                                             "0102030405060708090a0b0c0d0e0f10$"
                                                             "00000000000000000000000000000000"
                                                             "00000000000000000000000000000000"));
    // A corrupt record with plaintext present: re-hash the plaintext.
    TEST_ASSERT_EQUAL(Migration::HashPlaintext, web_pw::migration("legacy", "garbage"));
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
    RUN_TEST(test_migration_rules);
    return UNITY_END();
}
