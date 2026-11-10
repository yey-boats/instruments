// instruments#23: the device manifest truthfully advertises the MIDL source /
// action kinds THIS firmware implements, and documents that use any other kind
// are rejected with a clear error (instead of silently rendering "--" or
// dropping the action).
//
// The MIDL schema (the language) is a superset: it also allows `computed`
// sources. Per the MIDL design the per-target manifest says what a target
// supports, so the fix is a truthful manifest + an ingestion check, not
// implementing every schema kind.

#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>

#include "generated_midl_manifest.h"
#include "midl_kinds.h"

void setUp(void) {
}
void tearDown(void) {
}

static bool list_eq(JsonArrayConst arr, const char *const *expect) {
    size_t i = 0;
    for (JsonVariantConst v : arr) {
        if (!expect[i] || strcmp(v.as<const char *>(), expect[i]) != 0) return false;
        ++i;
    }
    return expect[i] == nullptr;
}

static void test_firmware_kinds() {
    // What midl_render.cpp actually maps: signalk paths, local metric ids,
    // const literals; nav, command, and put (SignalK PUT).
    const char *src[] = {"signalk", "local", "const", nullptr};
    const char *act[] = {"nav", "command", "put", nullptr};
    for (int i = 0; src[i]; ++i)
        TEST_ASSERT_EQUAL_STRING(src[i], midl::kinds::SOURCES[i]);
    TEST_ASSERT_NULL(midl::kinds::SOURCES[3]);
    for (int i = 0; act[i]; ++i)
        TEST_ASSERT_EQUAL_STRING(act[i], midl::kinds::ACTION_KINDS[i]);
    TEST_ASSERT_NULL(midl::kinds::ACTION_KINDS[3]);
    TEST_ASSERT_FALSE(midl::kinds::source_supported("computed"));
    TEST_ASSERT_FALSE(midl::kinds::source_supported(""));
    TEST_ASSERT_FALSE(midl::kinds::action_supported("launch"));
}

static void test_embedded_manifest_advertises_exactly_the_firmware_kinds() {
    JsonDocument m;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(m, midl_manifest::JSON).code());
    TEST_ASSERT_TRUE_MESSAGE(list_eq(m["sources"].as<JsonArrayConst>(), midl::kinds::SOURCES),
                             "manifest.sources != midl::kinds::SOURCES");
    TEST_ASSERT_TRUE_MESSAGE(
        list_eq(m["actionKinds"].as<JsonArrayConst>(), midl::kinds::ACTION_KINDS),
        "manifest.actionKinds != midl::kinds::ACTION_KINDS");
}

static bool check(const char *json, char *err, size_t n) {
    JsonDocument d;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, json).code());
    return midl::kinds::check_doc(d.as<JsonVariantConst>(), err, n);
}

static void test_supported_doc_accepted() {
    char err[160] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "sog":{"type":"single-value","bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}},
        "ip":{"type":"text","bindings":{"value":{"kind":"local","id":"net.ip"}}},
        "k":{"type":"text","bindings":{"value":{"kind":"const","value":"MOORED"}}},
        "legacy":{"type":"single-value","bindings":{"value":{"path":"environment.depth.belowKeel"}}},
        "c":{"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingMagnetic"}},
             "markers":[{"glyph":"triangle","dir":{"kind":"const","value":45}}]},
        "go":{"type":"button","action":{"kind":"nav","target":"b"}},
        "cmd":{"type":"button","action":{"kind":"command","target":"view wind"}},
        "tack":{"type":"button","action":{"kind":"put","target":"steering.autopilot.tack","value":true}}
      },"layout":{"element":"sog"}}],
      "alarms":[{"id":"shallow","level":"alarm","message":"x","lt":2,
                 "source":{"kind":"signalk","path":"environment.depth.belowKeel"}}]})";
    TEST_ASSERT_TRUE_MESSAGE(check(doc, err, sizeof(err)), err);
}

static void test_computed_binding_rejected_with_path() {
    char err[160] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "vmg":{"type":"single-value","bindings":{"value":{"kind":"computed","expr":"sog * 0.5"}}}
      },"layout":{"element":"vmg"}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "computed"));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/vmg/bindings/value"));
    TEST_ASSERT_NOT_NULL(strstr(err, "signalk, local, const"));
}

static void test_computed_marker_and_alarm_rejected() {
    char err[160] = "";
    const char *marker = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "c":{"type":"compass","markers":[{"dir":{"kind":"computed","expr":"x"}}]}}}]})";
    TEST_ASSERT_FALSE(check(marker, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/c/markers/0/dir"));
    const char *alarm = R"({"midl":"1.0.0","screens":[],
        "alarms":[{"id":"x","source":{"kind":"computed","expr":"y"}}]})";
    TEST_ASSERT_FALSE(check(alarm, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "/alarms/0/source"));
}

static void test_unknown_action_kind_rejected() {
    char err[160] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"launch","target":"x"}}}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "launch"));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/b/action"));
    TEST_ASSERT_NOT_NULL(strstr(err, "nav, command, put"));
}

// put writes a SignalK path; a non-path target must not fall through to the
// console-command funnel.
static void test_put_requires_signalk_path_target() {
    char err[160] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"put","target":"wifi-forget","value":true}}}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "put"));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/b/action"));
}

// Review of #29: put_target_ok must accept only a real SignalK path — it is
// concatenated into the PUT URL that carries the device's bearer token.
static void test_put_target_ok_is_a_signalk_path() {
    using midl::kinds::put_target_ok;
    TEST_ASSERT_TRUE(put_target_ok("steering.autopilot.tack"));
    TEST_ASSERT_TRUE(put_target_ok("electrical.batteries.1.voltage"));
    TEST_ASSERT_TRUE(put_target_ok("propulsion.port_eng.state"));
    TEST_ASSERT_TRUE(put_target_ok("electrical.switches.bank-1.state"));
    TEST_ASSERT_TRUE(put_target_ok("steering.autopilot.target.headingMagnetic"));
    const char *bad[] = {"",           "nodot", "../../x.y",   "a.b?x", "a.b#c", "a..b",   ".a.b",
                         "a.b.",       "a.b\t", "a.b\nc",      "a b.c", "a/b.c", "a.b%2e", "a.b&c",
                         "a.\xc3\xa9", "a.b:c", "sk 10.0.0.1", "a.b;c", "a.b+c", "a.b@c"};
    for (const char *b : bad)
        TEST_ASSERT_FALSE_MESSAGE(put_target_ok(b), b);
    TEST_ASSERT_FALSE(put_target_ok(nullptr));
}

// The target buffer holds ACTION_TARGET_MAX chars; a longer target is rejected
// whole (never truncated into something else).
static void test_put_target_ok_bounded() {
    char ok[midl::kinds::ACTION_TARGET_MAX + 1];
    memset(ok, 'a', sizeof(ok) - 1);
    ok[sizeof(ok) - 1] = 0;
    ok[5] = '.';
    TEST_ASSERT_TRUE(midl::kinds::put_target_ok(ok));
    char longer[midl::kinds::ACTION_TARGET_MAX + 2];
    memset(longer, 'a', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = 0;
    longer[5] = '.';
    TEST_ASSERT_FALSE(midl::kinds::put_target_ok(longer));
}

static void test_put_rejects_injection_targets() {
    char err[200] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"put","target":"../../../x.y","value":true}}}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/b/action"));
}

// Real SignalK autopilot paths exceed 31 chars and are accepted whole; a
// target longer than the stored buffer is rejected, never accepted and PUT
// to a truncated path.
static void test_long_put_target_rejected() {
    char err[200] = "";
    const char *ok = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"put","target":"steering.autopilot.target.headingMagnetic"}}}}]})";
    TEST_ASSERT_TRUE_MESSAGE(check(ok, err, sizeof(err)), err);
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"put","target":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b"}}}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/b/action"));
    TEST_ASSERT_NOT_NULL(strstr(err, "95"));
}

// A command target longer than the buffer is rejected too: truncation could
// otherwise turn it into a different command (or a different PUT path).
static void test_long_command_target_rejected() {
    char err[200] = "";
    const char *doc = R"({"midl":"1.0.0","screens":[{"id":"a","elements":{
        "b":{"type":"button","action":{"kind":"command","target":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b"}}}}]})";
    TEST_ASSERT_FALSE(check(doc, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "/screens/0/elements/b/action"));
}

// sk::putValue URL-encodes the path segment anyway (defence in depth).
static void test_put_url_path_encoding() {
    char out[64];
    TEST_ASSERT_TRUE(midl::kinds::encode_put_path("steering.autopilot.tack", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.tack", out);
    TEST_ASSERT_TRUE(midl::kinds::encode_put_path("steering/autopilot/state", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("steering/autopilot/state", out);
    TEST_ASSERT_TRUE(midl::kinds::encode_put_path("a.b?x#y z%", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a.b%3Fx%23y%20z%25", out);
    TEST_ASSERT_FALSE(midl::kinds::encode_put_path("abcdefgh", out, 8));
    TEST_ASSERT_FALSE(midl::kinds::encode_put_path(nullptr, out, sizeof(out)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_firmware_kinds);
    RUN_TEST(test_embedded_manifest_advertises_exactly_the_firmware_kinds);
    RUN_TEST(test_supported_doc_accepted);
    RUN_TEST(test_computed_binding_rejected_with_path);
    RUN_TEST(test_computed_marker_and_alarm_rejected);
    RUN_TEST(test_unknown_action_kind_rejected);
    RUN_TEST(test_put_requires_signalk_path_target);
    RUN_TEST(test_put_target_ok_is_a_signalk_path);
    RUN_TEST(test_put_target_ok_bounded);
    RUN_TEST(test_put_rejects_injection_targets);
    RUN_TEST(test_long_put_target_rejected);
    RUN_TEST(test_long_command_target_rejected);
    RUN_TEST(test_put_url_path_encoding);
    return UNITY_END();
}
