#include <unity.h>
#include <cstring>
#include "layout.h"

using namespace layout;

void setUp(void) {
}
void tearDown(void) {
}

static const char *MINIMAL_JSON =
    "{"
    "\"version\":1,"
    "\"settings\":{\"default_screen\":\"dash\",\"demo_period_ms\":2500},"
    "\"screens\":[{"
    "  \"id\":\"dash\",\"title\":\"Dashboard\",\"type\":\"quadrants\","
    "  \"tiles\":["
    "    {\"id\":\"w\",\"title\":\"WIND\",\"type\":\"wind\","
    "     \"paths\":{\"awa\":\"environment.wind.angleApparent\","
    "                \"aws\":\"environment.wind.speedApparent\"}},"
    "    {\"id\":\"n\",\"title\":\"NAV\",\"type\":\"nav\"}"
    "  ]"
    "}],"
    "\"alarms\":["
    "  {\"id\":\"shallow\",\"path\":\"environment.depth.belowTransducer\","
    "   \"level\":\"alarm\",\"lt\":3.0,\"message\":\"SHALLOW WATER\"}"
    "]"
    "}";

static void test_parse_minimal() {
    Config c;
    int rc = parse(MINIMAL_JSON, strlen(MINIMAL_JSON), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(1, c.version);
    TEST_ASSERT_EQUAL_STRING("dash", c.settings.default_screen);
    TEST_ASSERT_EQUAL_UINT32(2500, c.settings.demo_period_ms);
    TEST_ASSERT_EQUAL(1, c.screen_count);
    TEST_ASSERT_EQUAL_STRING("dash", c.screens[0].id);
    TEST_ASSERT_EQUAL(SCREEN_QUADRANTS, c.screens[0].type);
    TEST_ASSERT_EQUAL(2, c.screens[0].tile_count);
}

static void test_tile_types_and_paths() {
    Config c;
    parse(MINIMAL_JSON, strlen(MINIMAL_JSON), c);
    const Tile &t = c.screens[0].tiles[0];
    TEST_ASSERT_EQUAL_STRING("w", t.id);
    TEST_ASSERT_EQUAL_STRING("WIND", t.title);
    TEST_ASSERT_EQUAL(TILE_WIND, t.type);
    TEST_ASSERT_EQUAL(2, t.path_count);
    // Order of keys in JSON objects is insertion-order in ArduinoJson 7.
    TEST_ASSERT_EQUAL_STRING("awa", t.paths[0].key);
    TEST_ASSERT_EQUAL_STRING("environment.wind.angleApparent", t.paths[0].path);
    TEST_ASSERT_EQUAL_STRING("aws", t.paths[1].key);
    TEST_ASSERT_EQUAL(TILE_NAV, c.screens[0].tiles[1].type);
    TEST_ASSERT_EQUAL(0, c.screens[0].tiles[1].path_count);
}

static void test_parse_alarm_rule() {
    Config c;
    parse(MINIMAL_JSON, strlen(MINIMAL_JSON), c);
    TEST_ASSERT_EQUAL(1, c.alarm_count);
    const AlarmRule &a = c.alarms[0];
    TEST_ASSERT_EQUAL_STRING("shallow", a.id);
    TEST_ASSERT_EQUAL_STRING("environment.depth.belowTransducer", a.path);
    TEST_ASSERT_EQUAL(ALARM_LVL_ALARM, a.level);
    TEST_ASSERT_TRUE(a.has_lt);
    TEST_ASSERT_FALSE(a.has_gt);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 3.0, a.lt);
    TEST_ASSERT_EQUAL_STRING("SHALLOW WATER", a.message);
}

static void test_unknown_types_default_to_unknown_enum() {
    const char *j = "{\"screens\":[{\"id\":\"x\",\"type\":\"made-up\","
                    "\"tiles\":[{\"id\":\"y\",\"type\":\"also-made-up\"}]}]}";
    Config c;
    parse(j, strlen(j), c);
    TEST_ASSERT_EQUAL(1, c.screen_count);
    TEST_ASSERT_EQUAL(SCREEN_UNKNOWN, c.screens[0].type);
    TEST_ASSERT_EQUAL(TILE_UNKNOWN, c.screens[0].tiles[0].type);
}

static void test_find_screen() {
    Config c;
    parse(MINIMAL_JSON, strlen(MINIMAL_JSON), c);
    const Screen *s = find_screen(c, "dash");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(SCREEN_QUADRANTS, s->type);
    TEST_ASSERT_NULL(find_screen(c, "nonexistent"));
}

static void test_invalid_json_returns_error() {
    const char *bad = "{not valid";
    Config c;
    int rc = parse(bad, strlen(bad), c);
    TEST_ASSERT_LESS_THAN(0, rc);
}

static void test_empty_object_is_valid() {
    const char *j = "{}";
    Config c;
    int rc = parse(j, strlen(j), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(0, c.screen_count);
    TEST_ASSERT_EQUAL(0, c.alarm_count);
    TEST_ASSERT_EQUAL_STRING("", c.settings.default_screen);
}

static void test_too_many_screens_are_clamped() {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "{\"screens\":[");
    for (size_t i = 0; i < MAX_SCREENS + 3; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"id\":\"s%u\",\"type\":\"quadrants\"}",
                      i == 0 ? "" : ",", (unsigned)i);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    Config c;
    int rc = parse(buf, strlen(buf), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(MAX_SCREENS, c.screen_count);
}

static void test_long_strings_are_truncated_safely() {
    const char *j =
        "{\"screens\":[{\"id\":\"thisIsADeliberatelyLongIdentifierThatExceedsTheFixedBufferOf32\","
        "\"type\":\"quadrants\"}]}";
    Config c;
    parse(j, strlen(j), c);
    TEST_ASSERT_EQUAL(1, c.screen_count);
    TEST_ASSERT_EQUAL(STR_LEN - 1, strlen(c.screens[0].id));  // truncated, NUL-terminated
}

static void test_apply_resets_previous_config() {
    // First parse leaves screens / alarms populated.
    Config c;
    parse(MINIMAL_JSON, strlen(MINIMAL_JSON), c);
    TEST_ASSERT_EQUAL(1, c.screen_count);
    TEST_ASSERT_EQUAL(1, c.alarm_count);
    // Second parse with empty doc should clear back to zero state.
    parse("{}", 2, c);
    TEST_ASSERT_EQUAL(0, c.screen_count);
    TEST_ASSERT_EQUAL(0, c.alarm_count);
    TEST_ASSERT_EQUAL_STRING("", c.settings.default_screen);
}

static void test_tile_path_count_clamps_at_max() {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"screens\":[{\"id\":\"s\",\"type\":\"quadrants\","
                     "\"tiles\":[{\"id\":\"t\",\"type\":\"big_number\",\"paths\":{");
    for (size_t i = 0; i < MAX_PATHS_PER_OBJECT + 3; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s\"k%u\":\"p%u\"", i ? "," : "", (unsigned)i,
                      (unsigned)i);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "}}]}]}");
    Config c;
    int rc = parse(buf, strlen(buf), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(MAX_PATHS_PER_OBJECT, c.screens[0].tiles[0].path_count);
}

static void test_alarm_gt_only() {
    const char *j = "{\"alarms\":[{\"id\":\"hot\",\"path\":\"environment.water.temperature\","
                    "\"level\":\"warn\",\"gt\":300.0,\"message\":\"HOT WATER\"}]}";
    Config c;
    int rc = parse(j, strlen(j), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(1, c.alarm_count);
    const AlarmRule &a = c.alarms[0];
    TEST_ASSERT_FALSE(a.has_lt);
    TEST_ASSERT_TRUE(a.has_gt);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 300.0, a.gt);
    TEST_ASSERT_EQUAL(ALARM_LVL_WARN, a.level);
}

static void test_all_screen_types() {
    static const char *types[] = {"quadrants", "steering", "autopilot", "route", "trip", "chart"};
    static const ScreenType expected[] = {SCREEN_QUADRANTS, SCREEN_STEERING, SCREEN_AUTOPILOT,
                                          SCREEN_ROUTE,     SCREEN_TRIP,     SCREEN_CHART};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"screens\":[{\"id\":\"s\",\"type\":\"%s\"}]}", types[i]);
        Config c;
        parse(buf, strlen(buf), c);
        TEST_ASSERT_EQUAL(1, c.screen_count);
        TEST_ASSERT_EQUAL_MESSAGE(expected[i], c.screens[0].type, types[i]);
    }
}

static void test_all_alarm_levels() {
    static const char *levels[] = {"info", "warn", "alarm", "emergency"};
    static const AlarmLevel expected[] = {ALARM_LVL_INFO, ALARM_LVL_WARN, ALARM_LVL_ALARM,
                                          ALARM_LVL_EMERGENCY};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"alarms\":[{\"id\":\"a\",\"path\":\"x\",\"level\":\"%s\",\"lt\":1}]}",
                 levels[i]);
        Config c;
        parse(buf, strlen(buf), c);
        TEST_ASSERT_EQUAL(1, c.alarm_count);
        TEST_ASSERT_EQUAL_MESSAGE(expected[i], c.alarms[0].level, levels[i]);
    }
}

static void test_too_many_alarms_are_clamped() {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "{\"alarms\":[");
    for (size_t i = 0; i < MAX_ALARMS + 2; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"id\":\"a%u\",\"path\":\"p%u\",\"level\":\"warn\",\"lt\":1}",
                      i ? "," : "", (unsigned)i, (unsigned)i);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    Config c;
    int rc = parse(buf, strlen(buf), c);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(MAX_ALARMS, c.alarm_count);
}

static void test_screen_paths_are_parsed() {
    const char *j = "{\"screens\":[{\"id\":\"steer\",\"type\":\"steering\","
                    "\"paths\":{\"hdg\":\"navigation.headingTrue\","
                    "           \"cts\":\"navigation.courseRhumbline.bearingTrackTrue\","
                    "           \"xte\":\"navigation.courseRhumbline.crossTrackError\"}}]}";
    Config c;
    parse(j, strlen(j), c);
    TEST_ASSERT_EQUAL(1, c.screen_count);
    TEST_ASSERT_EQUAL(3, c.screens[0].path_count);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_minimal);
    RUN_TEST(test_tile_types_and_paths);
    RUN_TEST(test_parse_alarm_rule);
    RUN_TEST(test_unknown_types_default_to_unknown_enum);
    RUN_TEST(test_find_screen);
    RUN_TEST(test_invalid_json_returns_error);
    RUN_TEST(test_empty_object_is_valid);
    RUN_TEST(test_too_many_screens_are_clamped);
    RUN_TEST(test_long_strings_are_truncated_safely);
    RUN_TEST(test_apply_resets_previous_config);
    RUN_TEST(test_tile_path_count_clamps_at_max);
    RUN_TEST(test_alarm_gt_only);
    RUN_TEST(test_all_screen_types);
    RUN_TEST(test_all_alarm_levels);
    RUN_TEST(test_too_many_alarms_are_clamped);
    RUN_TEST(test_screen_paths_are_parsed);
    return UNITY_END();
}
