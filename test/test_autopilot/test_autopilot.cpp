#include <unity.h>
#include <cstring>

#include "autopilot.h"

using namespace autopilot;

void setUp(void) {
}
void tearDown(void) {
}

static void test_mode_name_returns_string_for_each_mode() {
    TEST_ASSERT_EQUAL_STRING("standby", mode_name(Mode::Standby));
    TEST_ASSERT_EQUAL_STRING("auto", mode_name(Mode::Auto));
    TEST_ASSERT_EQUAL_STRING("wind", mode_name(Mode::Wind));
    TEST_ASSERT_EQUAL_STRING("pretrack", mode_name(Mode::PreTrack));
    TEST_ASSERT_EQUAL_STRING("track", mode_name(Mode::Track));
    TEST_ASSERT_EQUAL_STRING("unknown", mode_name(Mode::Unknown));
}

static void test_mode_from_string_known_values() {
    TEST_ASSERT_EQUAL_INT((int)Mode::Standby, (int)mode_from_string("standby"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Auto, (int)mode_from_string("auto"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Wind, (int)mode_from_string("wind"));
    TEST_ASSERT_EQUAL_INT((int)Mode::PreTrack, (int)mode_from_string("pretrack"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Track, (int)mode_from_string("track"));
}

static void test_mode_from_string_unknown_returns_unknown() {
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown, (int)mode_from_string("nonsense"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown, (int)mode_from_string(""));
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown, (int)mode_from_string(nullptr));
}

static void test_mode_round_trip() {
    Mode modes[] = {Mode::Standby, Mode::Auto, Mode::Wind, Mode::PreTrack, Mode::Track};
    for (Mode m : modes) {
        const char *s = mode_name(m);
        Mode back = mode_from_string(s);
        TEST_ASSERT_EQUAL_INT((int)m, (int)back);
    }
}

static void test_backend_name() {
    TEST_ASSERT_EQUAL_STRING("signalk", backend_name(Backend::SignalK));
    TEST_ASSERT_EQUAL_STRING("nmea2000", backend_name(Backend::NMEA2000Raymarine));
}

static void test_result_name() {
    TEST_ASSERT_EQUAL_STRING("ok", result_name(Result::Ok));
    TEST_ASSERT_EQUAL_STRING("invalid", result_name(Result::InvalidPayload));
    TEST_ASSERT_EQUAL_STRING("backend-unavailable", result_name(Result::BackendUnavailable));
    TEST_ASSERT_EQUAL_STRING("failed", result_name(Result::Failed));
    TEST_ASSERT_EQUAL_STRING("forbidden", result_name(Result::Forbidden));
}

// ---- spec 17 §6 permissions ---------------------------------------------

static void test_mode_allowed_default_permissions() {
    Permissions p{};  // engage=false, standby=true, heading_adjust=true
    TEST_ASSERT_FALSE(mode_allowed(Mode::Unknown, p));
    TEST_ASSERT_TRUE(mode_allowed(Mode::Standby, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::Auto, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::Wind, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::PreTrack, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::Track, p));
}

static void test_mode_allowed_engage_open() {
    Permissions p{};
    p.allow_engage = true;
    TEST_ASSERT_TRUE(mode_allowed(Mode::Auto, p));
    TEST_ASSERT_TRUE(mode_allowed(Mode::Wind, p));
    TEST_ASSERT_TRUE(mode_allowed(Mode::PreTrack, p));
    TEST_ASSERT_TRUE(mode_allowed(Mode::Track, p));
    // Standby uses its own gate, still open by default.
    TEST_ASSERT_TRUE(mode_allowed(Mode::Standby, p));
    // Unknown is never allowed, even with engage open.
    TEST_ASSERT_FALSE(mode_allowed(Mode::Unknown, p));
}

static void test_mode_allowed_standby_locked() {
    Permissions p{};
    p.allow_engage = false;
    p.allow_standby = false;
    TEST_ASSERT_FALSE(mode_allowed(Mode::Standby, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::Auto, p));
}

static void test_mode_allowed_engage_open_standby_locked() {
    // Plausible operator config: under-way under autopilot, standby
    // is locked out so the helm can't accidentally drop authority.
    Permissions p{};
    p.allow_engage = true;
    p.allow_standby = false;
    TEST_ASSERT_TRUE(mode_allowed(Mode::Auto, p));
    TEST_ASSERT_FALSE(mode_allowed(Mode::Standby, p));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_mode_name_returns_string_for_each_mode);
    RUN_TEST(test_mode_from_string_known_values);
    RUN_TEST(test_mode_from_string_unknown_returns_unknown);
    RUN_TEST(test_mode_round_trip);
    RUN_TEST(test_backend_name);
    RUN_TEST(test_result_name);
    RUN_TEST(test_mode_allowed_default_permissions);
    RUN_TEST(test_mode_allowed_engage_open);
    RUN_TEST(test_mode_allowed_standby_locked);
    RUN_TEST(test_mode_allowed_engage_open_standby_locked);
    return UNITY_END();
}
