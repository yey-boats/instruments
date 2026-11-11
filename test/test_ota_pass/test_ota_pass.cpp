#include <string.h>
#include <unity.h>
#include "ota_pass.h"
#include "ota_policy.h"
void setUp() {
}
void tearDown() {
}
static void test_nvs_wins() {
    TEST_ASSERT_EQUAL_STRING("rt", ota_pass_effective("rt", "compiled"));
}
static void test_empty_nvs_falls_back() {
    TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective("", "compiled"));
}
static void test_null_nvs_falls_back() {
    TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective(nullptr, "compiled"));
}
static void test_both_empty() {
    TEST_ASSERT_EQUAL_STRING("", ota_pass_effective("", ""));
}
// instruments#30: ArduinoOTA (port 3232) policy.
//   release build          -> Disabled (the manager's pull OTA covers updates)
//   dev build, password    -> Password
//   dev build, no password -> Disabled, unless built with the lab-only
//                             YEYBOATS_LAB_OPEN_OTA flag (LabOpen)
static void test_release_never_runs_arduino_ota() {
    using ota_policy::Mode;
    TEST_ASSERT_EQUAL(Mode::Disabled, ota_policy::mode(true, false, "pw"));
    TEST_ASSERT_EQUAL(Mode::Disabled, ota_policy::mode(true, false, ""));
    TEST_ASSERT_EQUAL(Mode::Disabled, ota_policy::mode(true, true, ""));
}
static void test_dev_with_password() {
    using ota_policy::Mode;
    TEST_ASSERT_EQUAL(Mode::Password, ota_policy::mode(false, false, "pw"));
    TEST_ASSERT_EQUAL(Mode::Password, ota_policy::mode(false, true, "pw"));
}
static void test_dev_without_password_is_closed_unless_lab() {
    using ota_policy::Mode;
    TEST_ASSERT_EQUAL(Mode::Disabled, ota_policy::mode(false, false, ""));
    TEST_ASSERT_EQUAL(Mode::Disabled, ota_policy::mode(false, false, nullptr));
    TEST_ASSERT_EQUAL(Mode::LabOpen, ota_policy::mode(false, true, ""));
}
static void test_mode_names() {
    TEST_ASSERT_EQUAL_STRING("disabled", ota_policy::mode_name(ota_policy::Mode::Disabled));
    TEST_ASSERT_EQUAL_STRING("password", ota_policy::mode_name(ota_policy::Mode::Password));
    TEST_ASSERT_EQUAL_STRING("lab-open", ota_policy::mode_name(ota_policy::Mode::LabOpen));
}
int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nvs_wins);
    RUN_TEST(test_empty_nvs_falls_back);
    RUN_TEST(test_null_nvs_falls_back);
    RUN_TEST(test_both_empty);
    RUN_TEST(test_release_never_runs_arduino_ota);
    RUN_TEST(test_dev_with_password);
    RUN_TEST(test_dev_without_password_is_closed_unless_lab);
    RUN_TEST(test_mode_names);
    return UNITY_END();
}
