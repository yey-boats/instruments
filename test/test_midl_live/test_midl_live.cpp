// GET /api/midl/live pure half: query parsing + caps, JSON shape, watch set.
#include "midl_live.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unity.h>

using namespace midl::live;

void setUp() {
}
void tearDown() {
}

static KeyList g_list;  // a few KB: keep off the test stack too

static void test_valid_path() {
    TEST_ASSERT_TRUE(valid_path("navigation.speedOverGround"));
    TEST_ASSERT_TRUE(valid_path("electrical.batteries.house-1.voltage"));
    TEST_ASSERT_TRUE(valid_path("tanks.fuel.0.currentLevel"));
    TEST_ASSERT_FALSE(valid_path(""));
    TEST_ASSERT_FALSE(valid_path(nullptr));
    TEST_ASSERT_FALSE(valid_path("notifications.*"));
    TEST_ASSERT_FALSE(valid_path(".navigation"));
    TEST_ASSERT_FALSE(valid_path("navigation."));
    TEST_ASSERT_FALSE(valid_path("a..b"));
    TEST_ASSERT_FALSE(valid_path("a b"));
    TEST_ASSERT_FALSE(valid_path("a\"b"));
    std::string long79(79, 'a'), long80(80, 'a');
    TEST_ASSERT_TRUE(valid_path(long79.c_str()));
    TEST_ASSERT_FALSE(valid_path(long80.c_str()));
}

static void test_valid_local() {
    TEST_ASSERT_TRUE(valid_local("net.ip"));
    TEST_ASSERT_TRUE(valid_local("trip.avgSpeed"));
    TEST_ASSERT_TRUE(valid_local("device.heap"));
    TEST_ASSERT_FALSE(valid_local(""));
    TEST_ASSERT_FALSE(valid_local("a:b"));
    TEST_ASSERT_FALSE(valid_local(std::string(32, 'x').c_str()));
}

static void test_parse_basic_trim_dedupe() {
    parse_list(" navigation.speedOverGround,environment.depth.belowKeel ,,"
               "navigation.speedOverGround, bad path ,notifications.*",
               g_list, false);
    TEST_ASSERT_EQUAL_UINT(2, g_list.n);
    TEST_ASSERT_EQUAL_STRING("navigation.speedOverGround", g_list.key[0]);
    TEST_ASSERT_EQUAL_STRING("environment.depth.belowKeel", g_list.key[1]);
    TEST_ASSERT_EQUAL_UINT(2, g_list.invalid);
    TEST_ASSERT_FALSE(g_list.truncated);
}

static void test_parse_empty_and_null() {
    parse_list("", g_list, false);
    TEST_ASSERT_EQUAL_UINT(0, g_list.n);
    parse_list(nullptr, g_list, true);
    TEST_ASSERT_EQUAL_UINT(0, g_list.n);
    parse_list(",,,", g_list, true);
    TEST_ASSERT_EQUAL_UINT(0, g_list.n);
    TEST_ASSERT_EQUAL_UINT(0, g_list.invalid);
}

static void test_parse_cap() {
    std::string q;
    for (int i = 0; i < 40; ++i) {
        if (i) q += ",";
        q += "p.path" + std::to_string(i);
    }
    parse_list(q.c_str(), g_list, false);
    TEST_ASSERT_EQUAL_UINT(MAX_KEYS, g_list.n);
    TEST_ASSERT_TRUE(g_list.truncated);
    TEST_ASSERT_EQUAL_STRING("p.path0", g_list.key[0]);
    TEST_ASSERT_EQUAL_STRING("p.path31", g_list.key[31]);
}

static void test_parse_long_query_2kb() {
    // 32 paths of ~70 chars = ~2.3 KB: all accepted.
    std::string q;
    for (int i = 0; i < 32; ++i) {
        if (i) q += ",";
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "electrical.batteries.bank%02d.cells.cell%02d.voltage.extra.long.name", i, i);
        q += buf;
    }
    TEST_ASSERT_TRUE(q.size() > 2048);
    parse_list(q.c_str(), g_list, false);
    TEST_ASSERT_EQUAL_UINT(32, g_list.n);
    TEST_ASSERT_FALSE(g_list.truncated);
    TEST_ASSERT_EQUAL_UINT(0, g_list.invalid);
}

static void test_parse_overlong_param() {
    std::string q(MAX_PARAM + 100, 'a');
    parse_list(q.c_str(), g_list, false);
    TEST_ASSERT_EQUAL_UINT(0, g_list.n);
    TEST_ASSERT_TRUE(g_list.truncated);
    TEST_ASSERT_EQUAL_UINT(1, g_list.invalid);  // the (too long) item is dropped
}

static void test_parse_locals() {
    parse_list("net.ip,sys.uptime,trip.distance,bad:id", g_list, true);
    TEST_ASSERT_EQUAL_UINT(3, g_list.n);
    TEST_ASSERT_EQUAL_UINT(1, g_list.invalid);
}

class FakeResolver : public Resolver {
  public:
    void path(const char *p, Value &v) override {
        if (!strcmp(p, "navigation.speedOverGround")) {
            v.kind = Value::Number;
            v.num = 3.25;
        } else if (!strcmp(p, "steering.autopilot.state")) {
            v.kind = Value::Text;
            snprintf(v.text, sizeof(v.text), "auto");
        } else if (!strcmp(p, "environment.depth.belowKeel")) {
            v.kind = Value::Number;
            v.num = NAN;  // known but no data -> null
        }
    }
    void local(const char *id, Value &v) override {
        if (!strcmp(id, "net.ip")) {
            v.kind = Value::Text;
            snprintf(v.text, sizeof(v.text), "10.42.0.14");
        } else if (!strcmp(id, "heap")) {
            v.kind = Value::Number;
            v.num = 123;
        }
    }
};

static KeyList g_locals;

static void test_build_json_shape() {
    parse_list("navigation.speedOverGround,steering.autopilot.state,environment.depth.belowKeel,"
               "unknown.path",
               g_list, false);
    parse_list("net.ip,heap,nope", g_locals, true);
    FakeResolver r;
    JsonDocument doc;
    build_json(doc.to<JsonObject>(), 4242, g_list, g_locals, r);
    std::string out;
    serializeJson(doc, out);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":4242,\"values\":{\"navigation.speedOverGround\":3.25,"
        "\"steering.autopilot.state\":\"auto\",\"environment.depth.belowKeel\":null,"
        "\"unknown.path\":null},\"locals\":{\"net.ip\":\"10.42.0.14\",\"heap\":123,"
        "\"nope\":null}}",
        out.c_str());
}

static void test_build_json_empty_and_truncated() {
    parse_list("", g_list, false);
    parse_list("", g_locals, true);
    FakeResolver r;
    JsonDocument doc;
    build_json(doc.to<JsonObject>(), 1, g_list, g_locals, r);
    std::string out;
    serializeJson(doc, out);
    TEST_ASSERT_EQUAL_STRING("{\"t\":1,\"values\":{},\"locals\":{}}", out.c_str());

    g_list.truncated = true;
    JsonDocument doc2;
    build_json(doc2.to<JsonObject>(), 1, g_list, g_locals, r);
    TEST_ASSERT_TRUE(doc2["truncated"].as<bool>());
}

static WatchSet g_watch;

static void test_watch_touch_and_expire() {
    g_watch.clear();
    TEST_ASSERT_TRUE(g_watch.touch("a.b", 1000));
    TEST_ASSERT_FALSE(g_watch.touch("a.b", 2000));  // refresh, not a change
    TEST_ASSERT_TRUE(g_watch.touch("c.d", 5000));
    TEST_ASSERT_FALSE(g_watch.touch("x.*", 5000));  // invalid: refused
    TEST_ASSERT_EQUAL_UINT(2, g_watch.size());
    TEST_ASSERT_FALSE(g_watch.expire(12000));  // a.b last 2000: 10000 ms, not > TTL
    TEST_ASSERT_TRUE(g_watch.expire(12001));   // a.b expires
    TEST_ASSERT_EQUAL_UINT(1, g_watch.size());
    TEST_ASSERT_TRUE(g_watch.has("c.d"));
    TEST_ASSERT_FALSE(g_watch.has("a.b"));
    TEST_ASSERT_TRUE(g_watch.expire(15001));
    TEST_ASSERT_EQUAL_UINT(0, g_watch.size());
    TEST_ASSERT_FALSE(g_watch.expire(99999));
}

static void test_watch_evicts_oldest_when_full() {
    g_watch.clear();
    char p[32];
    for (unsigned i = 0; i < WatchSet::CAP; ++i) {
        snprintf(p, sizeof(p), "p.%u", i);
        TEST_ASSERT_TRUE(g_watch.touch(p, 100 + i));
    }
    g_watch.touch("p.0", 1000);  // p.0 is now the most recent; p.1 the oldest
    TEST_ASSERT_TRUE(g_watch.touch("p.new", 1001));
    TEST_ASSERT_EQUAL_UINT(WatchSet::CAP, g_watch.size());
    TEST_ASSERT_TRUE(g_watch.has("p.0"));
    TEST_ASSERT_FALSE(g_watch.has("p.1"));
    TEST_ASSERT_TRUE(g_watch.has("p.new"));
}

static void test_watch_millis_wrap() {
    g_watch.clear();
    TEST_ASSERT_TRUE(g_watch.touch("a.b", 0xFFFFF000u));
    TEST_ASSERT_FALSE(g_watch.expire(0x00000100u));  // 0x1100 ms later
    TEST_ASSERT_TRUE(g_watch.expire(0xFFFFF000u + WATCH_TTL_MS + 1));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_path);
    RUN_TEST(test_valid_local);
    RUN_TEST(test_parse_basic_trim_dedupe);
    RUN_TEST(test_parse_empty_and_null);
    RUN_TEST(test_parse_cap);
    RUN_TEST(test_parse_long_query_2kb);
    RUN_TEST(test_parse_overlong_param);
    RUN_TEST(test_parse_locals);
    RUN_TEST(test_build_json_shape);
    RUN_TEST(test_build_json_empty_and_truncated);
    RUN_TEST(test_watch_touch_and_expire);
    RUN_TEST(test_watch_evicts_oldest_when_full);
    RUN_TEST(test_watch_millis_wrap);
    return UNITY_END();
}
