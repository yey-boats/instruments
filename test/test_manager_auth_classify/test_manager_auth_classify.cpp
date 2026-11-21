#include <unity.h>

#include <string>

#include "manager_auth_classify.h"

using manager_auth_classify::classify_heartbeat_401;
using manager_auth_classify::HeartbeatAuthFailure;
using manager_auth_classify::reason_for;

void setUp(void) {
}

void tearDown(void) {
}

// instruments#91: SignalK security rejects the request before the plugin
// ever sees it (no/bad sk_token) - the body is its HTML login page, not
// the plugin's JSON error. The device token must be kept.
static void test_html_login_page_is_not_a_manager_rejection() {
    std::string html = "<!DOCTYPE html><html><head><title>Login</title></head>"
                       "<body><form action=\"/login\">...</form></body></html>";
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other, classify_heartbeat_401(html));
}

// The plugin's own structured rejection of the device token - the only
// shape that should clear the saved token.
static void test_plugin_unauthorized_error_is_a_manager_rejection() {
    std::string body = "{\"error\":{\"code\":\"unauthorized\",\"message\":\"bad device token\"}}";
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::ManagerRejectedToken, classify_heartbeat_401(body));
}

static void test_empty_body_is_not_a_manager_rejection() {
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other, classify_heartbeat_401(""));
}

static void test_non_json_garbage_is_not_a_manager_rejection() {
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other, classify_heartbeat_401("Unauthorized"));
}

// JSON that isn't shaped like the plugin's own error (e.g. some other
// endpoint's generic error, or a top-level string) must not be mistaken
// for a device-token rejection.
static void test_other_json_shapes_are_not_a_manager_rejection() {
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other,
                      classify_heartbeat_401("{\"error\":\"unauthorized\"}"));
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other, classify_heartbeat_401("{\"ok\":false}"));
}

// A plugin error with a different code (e.g. forbidden/rate-limited) is
// still a plugin response, but not the specific "your device token is
// invalid" signal - only "unauthorized" clears the token.
static void test_plugin_error_with_other_code_is_not_a_manager_rejection() {
    std::string body = "{\"error\":{\"code\":\"forbidden\",\"message\":\"device disabled\"}}";
    TEST_ASSERT_EQUAL(HeartbeatAuthFailure::Other, classify_heartbeat_401(body));
}

static void test_reason_strings_match_issue_wording() {
    TEST_ASSERT_EQUAL_STRING("manager rejected the device token",
                             reason_for(HeartbeatAuthFailure::ManagerRejectedToken));
    TEST_ASSERT_EQUAL_STRING("SignalK rejected the request: sk_token missing/invalid",
                             reason_for(HeartbeatAuthFailure::Other));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_html_login_page_is_not_a_manager_rejection);
    RUN_TEST(test_plugin_unauthorized_error_is_a_manager_rejection);
    RUN_TEST(test_empty_body_is_not_a_manager_rejection);
    RUN_TEST(test_non_json_garbage_is_not_a_manager_rejection);
    RUN_TEST(test_other_json_shapes_are_not_a_manager_rejection);
    RUN_TEST(test_plugin_error_with_other_code_is_not_a_manager_rejection);
    RUN_TEST(test_reason_strings_match_issue_wording);
    return UNITY_END();
}
