// Host tests for the URL helpers extracted from src/manager.cpp.
//
// These have caught real bugs in the past (the device kept appending
// /plugins/espdisp-manager to a URL that already had it, then the
// double-prefix base hit 404 silently). Pinning the behavior here
// stops a refactor from regressing it.

#include <unity.h>
#include <string>

#include "manager_url.h"

using manager_url::endpoint_has_path;
using manager_url::join_url;
using manager_url::plugin_base_from_root;

void setUp(void) {
}
void tearDown(void) {
}

// ---- endpoint_has_path ---------------------------------------------------

static void test_bare_authority_has_no_path() {
    TEST_ASSERT_FALSE(endpoint_has_path("http://host:3000"));
    TEST_ASSERT_FALSE(endpoint_has_path("https://signalk.local"));
    TEST_ASSERT_FALSE(endpoint_has_path("host:3000"));  // no scheme
    TEST_ASSERT_FALSE(endpoint_has_path(""));
}

static void test_trailing_slash_counts_as_path() {
    TEST_ASSERT_TRUE(endpoint_has_path("http://host:3000/"));
    TEST_ASSERT_TRUE(endpoint_has_path("http://host:3000/plugins/x"));
}

static void test_authority_with_userinfo_not_misread() {
    // "://" comes first; the colon inside :3000 must not confuse the
    // scheme detection.
    TEST_ASSERT_FALSE(endpoint_has_path("https://host:80"));
}

// ---- plugin_base_from_root -----------------------------------------------

static void test_appends_plugin_suffix_when_missing() {
    TEST_ASSERT_EQUAL_STRING("http://host:3000/plugins/espdisp-manager",
                             plugin_base_from_root("http://host:3000").c_str());
}

static void test_strips_trailing_slash_before_appending() {
    TEST_ASSERT_EQUAL_STRING("http://host:3000/plugins/espdisp-manager",
                             plugin_base_from_root("http://host:3000/").c_str());
    TEST_ASSERT_EQUAL_STRING("http://host:3000/plugins/espdisp-manager",
                             plugin_base_from_root("http://host:3000///").c_str());
}

static void test_does_not_double_append_when_already_present() {
    // Real bug we hit earlier - the device kept stacking the suffix on
    // every retry until the URL was unusable.
    TEST_ASSERT_EQUAL_STRING(
        "http://host:3000/plugins/espdisp-manager",
        plugin_base_from_root("http://host:3000/plugins/espdisp-manager").c_str());
    TEST_ASSERT_EQUAL_STRING(
        "http://host:3000/plugins/espdisp-manager",
        plugin_base_from_root("http://host:3000/plugins/espdisp-manager/").c_str());
}

static void test_idempotent_under_repeated_application() {
    std::string s = plugin_base_from_root("http://h:3000");
    for (int i = 0; i < 5; ++i) {
        s = plugin_base_from_root(s);
    }
    TEST_ASSERT_EQUAL_STRING("http://h:3000/plugins/espdisp-manager", s.c_str());
}

// ---- join_url ------------------------------------------------------------

static void test_join_exactly_one_slash() {
    TEST_ASSERT_EQUAL_STRING("http://h/devices/register",
                             join_url("http://h", "/devices/register").c_str());
    TEST_ASSERT_EQUAL_STRING("http://h/devices/register",
                             join_url("http://h/", "/devices/register").c_str());
}

static void test_join_caller_owns_leading_slash() {
    // join_url is intentionally not normalizing the path: if the
    // caller wants "/foo", they write "/foo"; if they want "foo",
    // they get "foo" stuck right after the base.
    TEST_ASSERT_EQUAL_STRING("http://hostdevices", join_url("http://host", "devices").c_str());
}

static void test_join_empty_path_returns_base() {
    TEST_ASSERT_EQUAL_STRING("http://host:3000", join_url("http://host:3000/", "").c_str());
}

static void test_join_null_path_returns_base() {
    TEST_ASSERT_EQUAL_STRING("http://host:3000", join_url("http://host:3000/", nullptr).c_str());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_bare_authority_has_no_path);
    RUN_TEST(test_trailing_slash_counts_as_path);
    RUN_TEST(test_authority_with_userinfo_not_misread);
    RUN_TEST(test_appends_plugin_suffix_when_missing);
    RUN_TEST(test_strips_trailing_slash_before_appending);
    RUN_TEST(test_does_not_double_append_when_already_present);
    RUN_TEST(test_idempotent_under_repeated_application);
    RUN_TEST(test_join_exactly_one_slash);
    RUN_TEST(test_join_caller_owns_leading_slash);
    RUN_TEST(test_join_empty_path_returns_base);
    RUN_TEST(test_join_null_path_returns_base);
    return UNITY_END();
}
