// The Settings "web login" row text (include/web_login_info.h).

#include <unity.h>

#include <string.h>

#include "web_login_info.h"

using web_auth_policy::State;
using web_login_info::describe;
using web_login_info::Input;

void setUp(void) {
}
void tearDown(void) {
}

static void test_enforced_paired_points_at_manager() {
    char who[64], where[64];
    describe(Input{State::Enforced, "yeyboats", "10.0.0.7", true}, who, sizeof(who), where,
             sizeof(where));
    TEST_ASSERT_EQUAL_STRING("user yeyboats, password: Display Manager device page", who);
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.7/editor", where);
}

static void test_enforced_unpaired_says_password_set() {
    char who[64], where[64];
    describe(Input{State::Enforced, "yeyboats", "10.0.0.7", false}, who, sizeof(who), where,
             sizeof(where));
    TEST_ASSERT_EQUAL_STRING("user yeyboats, password set", who);
}

static void test_missing_username_uses_firmware_default() {
    char who[64], where[64];
    describe(Input{State::Enforced, "", "10.0.0.7", true}, who, sizeof(who), where, sizeof(where));
    TEST_ASSERT_NOT_NULL(strstr(who, web_auth_policy::DEFAULT_USER));
    describe(Input{State::Enforced, nullptr, "10.0.0.7", true}, who, sizeof(who), where,
             sizeof(where));
    TEST_ASSERT_NOT_NULL(strstr(who, web_auth_policy::DEFAULT_USER));
}

static void test_setup_open_and_disabled() {
    char who[64], where[64];
    describe(Input{State::SetupOpen, "yeyboats", "10.0.0.7", false}, who, sizeof(who), where,
             sizeof(where));
    TEST_ASSERT_EQUAL_STRING("open: no password set", who);
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.7/editor", where);
    describe(Input{State::DisabledByOwner, "yeyboats", "10.0.0.7", true}, who, sizeof(who), where,
             sizeof(where));
    TEST_ASSERT_EQUAL_STRING("login off (web auth disabled)", who);
}

static void test_unknown_address() {
    TEST_ASSERT_FALSE(web_login_info::address_known(nullptr));
    TEST_ASSERT_FALSE(web_login_info::address_known(""));
    TEST_ASSERT_FALSE(web_login_info::address_known("0.0.0.0"));
    TEST_ASSERT_TRUE(web_login_info::address_known("192.168.4.1"));
    char who[64], where[64];
    describe(Input{State::Enforced, "u", "0.0.0.0", true}, who, sizeof(who), where, sizeof(where));
    TEST_ASSERT_EQUAL_STRING("no address yet (WiFi not connected)", where);
}

static void test_truncates_and_terminates() {
    char who[12], where[10];
    memset(who, 'x', sizeof(who));
    memset(where, 'x', sizeof(where));
    describe(Input{State::Enforced, "a-very-long-username-indeed", "192.168.100.200", true}, who,
             sizeof(who), where, sizeof(where));
    TEST_ASSERT_EQUAL_CHAR('\0', who[sizeof(who) - 1]);
    TEST_ASSERT_EQUAL_CHAR('\0', where[sizeof(where) - 1]);
    TEST_ASSERT_EQUAL_STRING_LEN("user a-ver", who, 10);
    // Null / zero-size outputs are tolerated.
    describe(Input{State::SetupOpen, "u", "1.2.3.4", false}, nullptr, 0, nullptr, 0);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_enforced_paired_points_at_manager);
    RUN_TEST(test_enforced_unpaired_says_password_set);
    RUN_TEST(test_missing_username_uses_firmware_default);
    RUN_TEST(test_setup_open_and_disabled);
    RUN_TEST(test_unknown_address);
    RUN_TEST(test_truncates_and_terminates);
    return UNITY_END();
}
