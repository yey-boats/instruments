// instruments#21: web API Basic Auth is ON by default once the device is set
// up. "Set up" means a web password has been provisioned (the manager pushes
// webAuth.{username,password}); there is no built-in shared password, so a
// factory-fresh / setup-AP device with no password stays open and first-boot
// provisioning keeps working. Only an explicit webAuth.enabled=false opts out.

#include <unity.h>

#include "web_auth_policy.h"

using namespace web_auth_policy;

void setUp(void) {
}
void tearDown(void) {
}

// NVS web/auth absent => the default flag is "on" (was "off" before #21).
static void test_default_flag_is_on() {
    TEST_ASSERT_EQUAL_UINT8(1, FLAG_DEFAULT);
}

static void test_provisioned_password_enforces_by_default() {
    TEST_ASSERT_TRUE(enforced(FLAG_DEFAULT, "s3cret-from-manager"));
    TEST_ASSERT_EQUAL(State::Enforced, state(FLAG_DEFAULT, "s3cret-from-manager"));
}

static void test_no_password_keeps_setup_open() {
    TEST_ASSERT_FALSE(enforced(FLAG_DEFAULT, ""));
    TEST_ASSERT_FALSE(enforced(FLAG_DEFAULT, nullptr));
    TEST_ASSERT_EQUAL(State::SetupOpen, state(FLAG_DEFAULT, ""));
    // Even an explicit "on" cannot lock out a device with no password.
    TEST_ASSERT_FALSE(enforced(1, ""));
}

static void test_explicit_opt_out_disables() {
    TEST_ASSERT_FALSE(enforced(0, "pw"));
    TEST_ASSERT_EQUAL(State::DisabledByOwner, state(0, "pw"));
}

static void test_state_names_are_stable() {
    // Surfaced in /api/security, mDNS TXT "auth" and the manager report.
    TEST_ASSERT_EQUAL_STRING("basic", state_name(State::Enforced));
    TEST_ASSERT_EQUAL_STRING("setup-open", state_name(State::SetupOpen));
    TEST_ASSERT_EQUAL_STRING("disabled", state_name(State::DisabledByOwner));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_default_flag_is_on);
    RUN_TEST(test_provisioned_password_enforces_by_default);
    RUN_TEST(test_no_password_keeps_setup_open);
    RUN_TEST(test_explicit_opt_out_disables);
    RUN_TEST(test_state_names_are_stable);
    return UNITY_END();
}
