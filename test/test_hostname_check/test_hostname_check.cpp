// Host tests for the hostname validator extracted from manager
// apply_config. The rules are intentionally a strict subset of RFC
// 1123 host labels; pin them here so a refactor can't loosen the
// surface that the plugin / BLE / web hostname-set paths share.

#include <unity.h>
#include <string.h>

#include "hostname_check.h"

using hostname_check::is_valid;

void setUp(void) {}
void tearDown(void) {}

static void test_typical_names_accepted() {
    TEST_ASSERT_TRUE(is_valid("espdisp"));
    TEST_ASSERT_TRUE(is_valid("espdisp-device"));
    TEST_ASSERT_TRUE(is_valid("dash"));
    TEST_ASSERT_TRUE(is_valid("Helm-1"));
    TEST_ASSERT_TRUE(is_valid("ESP32-board-3"));
    TEST_ASSERT_TRUE(is_valid("a"));  // single char, allowed
    TEST_ASSERT_TRUE(is_valid("0"));
}

static void test_null_and_empty_rejected() {
    TEST_ASSERT_FALSE(is_valid(nullptr));
    TEST_ASSERT_FALSE(is_valid(""));
}

static void test_max_length_boundary() {
    char ok31[32];
    memset(ok31, 'a', 31);
    ok31[31] = '\0';
    TEST_ASSERT_TRUE(is_valid(ok31));

    char too_long[33];
    memset(too_long, 'a', 32);
    too_long[32] = '\0';
    TEST_ASSERT_FALSE(is_valid(too_long));
}

static void test_leading_or_trailing_dash_rejected() {
    TEST_ASSERT_FALSE(is_valid("-foo"));
    TEST_ASSERT_FALSE(is_valid("foo-"));
    TEST_ASSERT_FALSE(is_valid("-"));
    // But internal dashes are fine.
    TEST_ASSERT_TRUE(is_valid("a-b-c"));
}

static void test_disallowed_chars_rejected() {
    TEST_ASSERT_FALSE(is_valid("foo bar"));         // space
    TEST_ASSERT_FALSE(is_valid("foo_bar"));         // underscore
    TEST_ASSERT_FALSE(is_valid("foo.bar"));         // dot
    TEST_ASSERT_FALSE(is_valid("foo/bar"));         // slash
    TEST_ASSERT_FALSE(is_valid("foo:bar"));         // colon
    TEST_ASSERT_FALSE(is_valid("foo@bar"));         // at
    TEST_ASSERT_FALSE(is_valid("voil\xc3\xa0"));    // utf8 (à)
    TEST_ASSERT_FALSE(is_valid("foo\tbar"));        // tab
    TEST_ASSERT_FALSE(is_valid("foo\nbar"));        // newline
}

static void test_mixed_case_accepted() {
    // Spec doesn't fold case; the apply path will lowercase before
    // passing to mDNS. Mixed-case input is still valid here.
    TEST_ASSERT_TRUE(is_valid("EspDisp"));
    TEST_ASSERT_TRUE(is_valid("ABCDEFG"));
}

static void test_digits_only_accepted() {
    TEST_ASSERT_TRUE(is_valid("12345"));
    // Long enough to test the boundary too.
    TEST_ASSERT_TRUE(is_valid("1234567890"));
}

static void test_known_problematic_inputs() {
    // Strings the plugin / operators have sent that must be rejected
    // before they reach the `id <name>` dispatch + reboot.
    TEST_ASSERT_FALSE(is_valid("null!"));        // bang
    TEST_ASSERT_FALSE(is_valid("\xff\xfe"));     // raw non-ASCII bytes
    TEST_ASSERT_FALSE(is_valid("My Boat"));      // space
    TEST_ASSERT_FALSE(is_valid("device.local")); // already-qualified FQDN
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_typical_names_accepted);
    RUN_TEST(test_null_and_empty_rejected);
    RUN_TEST(test_max_length_boundary);
    RUN_TEST(test_leading_or_trailing_dash_rejected);
    RUN_TEST(test_disallowed_chars_rejected);
    RUN_TEST(test_mixed_case_accepted);
    RUN_TEST(test_digits_only_accepted);
    RUN_TEST(test_known_problematic_inputs);
    return UNITY_END();
}
