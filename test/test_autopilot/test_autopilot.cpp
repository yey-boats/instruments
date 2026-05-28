#include <unity.h>
#include <cstring>

#include "autopilot.h"

using namespace autopilot;

void setUp(void) {}
void tearDown(void) {}

static void test_mode_name_returns_string_for_each_mode() {
    TEST_ASSERT_EQUAL_STRING("standby",  mode_name(Mode::Standby));
    TEST_ASSERT_EQUAL_STRING("auto",     mode_name(Mode::Auto));
    TEST_ASSERT_EQUAL_STRING("wind",     mode_name(Mode::Wind));
    TEST_ASSERT_EQUAL_STRING("pretrack", mode_name(Mode::PreTrack));
    TEST_ASSERT_EQUAL_STRING("track",    mode_name(Mode::Track));
    TEST_ASSERT_EQUAL_STRING("unknown",  mode_name(Mode::Unknown));
}

static void test_mode_from_string_known_values() {
    TEST_ASSERT_EQUAL_INT((int)Mode::Standby,  (int)mode_from_string("standby"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Auto,     (int)mode_from_string("auto"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Wind,     (int)mode_from_string("wind"));
    TEST_ASSERT_EQUAL_INT((int)Mode::PreTrack, (int)mode_from_string("pretrack"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Track,    (int)mode_from_string("track"));
}

static void test_mode_from_string_unknown_returns_unknown() {
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown,
                          (int)mode_from_string("nonsense"));
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown,
                          (int)mode_from_string(""));
    TEST_ASSERT_EQUAL_INT((int)Mode::Unknown,
                          (int)mode_from_string(nullptr));
}

static void test_mode_round_trip() {
    Mode modes[] = {Mode::Standby, Mode::Auto, Mode::Wind,
                    Mode::PreTrack, Mode::Track};
    for (Mode m : modes) {
        const char *s = mode_name(m);
        Mode back = mode_from_string(s);
        TEST_ASSERT_EQUAL_INT((int)m, (int)back);
    }
}

static void test_backend_name() {
    TEST_ASSERT_EQUAL_STRING("signalk",  backend_name(Backend::SignalK));
    TEST_ASSERT_EQUAL_STRING("nmea2000", backend_name(Backend::NMEA2000Raymarine));
}

static void test_result_name() {
    TEST_ASSERT_EQUAL_STRING("ok",                  result_name(Result::Ok));
    TEST_ASSERT_EQUAL_STRING("invalid",             result_name(Result::InvalidPayload));
    TEST_ASSERT_EQUAL_STRING("backend-unavailable", result_name(Result::BackendUnavailable));
    TEST_ASSERT_EQUAL_STRING("failed",              result_name(Result::Failed));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_mode_name_returns_string_for_each_mode);
    RUN_TEST(test_mode_from_string_known_values);
    RUN_TEST(test_mode_from_string_unknown_returns_unknown);
    RUN_TEST(test_mode_round_trip);
    RUN_TEST(test_backend_name);
    RUN_TEST(test_result_name);
    return UNITY_END();
}
