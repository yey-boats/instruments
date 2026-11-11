// instruments#20: on-device MIDL validation (midl::validate).
//
// The firmware must agree with midl's TypeScript and Python validators on
// every case of midl's conformance corpus (midl/conformance/cases.yaml +
// expected.json, exported to test/fixtures/yb-midl/conformance/cases.json by
// tools/gen_midl_conformance.py): same ok/reject verdict, and a rejection's
// location must be one of the issue paths the corpus records for that case.
// Beyond the corpus, the firmware's own storage limits reject documents the
// renderer would otherwise truncate.

#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "generated_midl_manifest.h"
#include "midl_demo_doc.h"
#include "midl_validate.h"

using midl::validate::Issue;

void setUp(void) {
}
void tearDown(void) {
}

static std::string slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

static const char *CORPUS = "test/fixtures/yb-midl/conformance/cases.json";

// Shallow-merge a case's manifestOverrides onto its class manifest (the corpus
// contract: same as midl's gen-expected.mjs / parity.py).
static void merged_manifest(JsonVariantConst base, JsonVariantConst overrides, JsonDocument &out) {
    out.set(base);
    for (JsonPairConst kv : overrides.as<JsonObjectConst>())
        out[kv.key()] = kv.value();
}

static bool path_in(JsonArrayConst paths, const char *p) {
    for (JsonVariantConst v : paths)
        if (strcmp(v | "", p) == 0) return true;
    return false;
}

static void test_corpus_agrees_with_midl_validators() {
    std::string text = slurp(CORPUS);
    JsonDocument corpus;
    TEST_ASSERT_EQUAL(
        DeserializationError::Ok,
        deserializeJson(corpus, text, DeserializationOption::NestingLimit(32)).code());
    JsonArrayConst cases = corpus["cases"].as<JsonArrayConst>();
    // midl#16: the 127-case corpus (midl 80b6410); never let it shrink.
    TEST_ASSERT_GREATER_OR_EQUAL(127, cases.size());

    std::string failures;
    size_t n = 0;
    for (JsonVariantConst c : cases) {
        const char *name = c["name"] | "?";
        const char *cls = c["targetClass"] | "";
        JsonDocument manifest;
        merged_manifest(corpus["manifests"][cls], c["manifestOverrides"], manifest);
        static Issue issue;
        memset(&issue, 0, sizeof(issue));
        bool ok = midl::validate::admit(c["doc"], manifest.as<JsonVariantConst>(), cls, issue);
        bool want = c["expected"]["ok"] | false;
        JsonArrayConst paths = c["expected"]["paths"].as<JsonArrayConst>();
        char line[512];
        line[0] = 0;
        if (ok != want) {
            snprintf(line, sizeof(line), "\n  %s [%s]: firmware %s, midl %s (%s: %s)", name, cls,
                     ok ? "ok" : "REJECT", want ? "ok" : "REJECT", issue.path, issue.message);
        } else if (!ok && !path_in(paths, issue.path)) {
            snprintf(line, sizeof(line), "\n  %s [%s]: rejected at %s (%s), midl paths differ",
                     name, cls, issue.path, issue.message);
        }
        failures += line;
        ++n;
    }
    TEST_ASSERT_EQUAL(cases.size(), n);
    TEST_ASSERT_TRUE_MESSAGE(failures.empty(), failures.c_str());
}

// The device's real embedded manifest (square-480 on native, with the
// firmware's own sources/actionKinds) gives the same verdicts as midl's
// generated square-480 manifest for every un-overridden square-480 case.
static void test_embedded_manifest_agrees_on_its_class() {
    std::string text = slurp(CORPUS);
    JsonDocument corpus;
    TEST_ASSERT_EQUAL(
        DeserializationError::Ok,
        deserializeJson(corpus, text, DeserializationOption::NestingLimit(32)).code());
    JsonDocument embedded;
    TEST_ASSERT_EQUAL(DeserializationError::Ok,
                      deserializeJson(embedded, midl_manifest::JSON).code());
    std::string failures;
    size_t checked = 0;
    for (JsonVariantConst c : corpus["cases"].as<JsonArrayConst>()) {
        if (strcmp(c["targetClass"] | "", midl_manifest::CLASS_ID) != 0) continue;
        if (c["manifestOverrides"].size() != 0) continue;
        static Issue issue;
        bool ok = midl::validate::admit(c["doc"], embedded.as<JsonVariantConst>(),
                                        midl_manifest::CLASS_ID, issue);
        if (ok != (c["expected"]["ok"] | false)) {
            failures += "\n  ";
            failures += c["name"] | "?";
        }
        ++checked;
    }
    TEST_ASSERT_GREATER_THAN(40, checked);
    TEST_ASSERT_TRUE_MESSAGE(failures.empty(), failures.c_str());
}

// --- helpers for hand-written documents -----------------------------------

static bool admit_json(const char *json, Issue &issue) {
    JsonDocument doc;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, json).code());
    JsonDocument m;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(m, midl_manifest::JSON).code());
    memset(&issue, 0, sizeof(issue));
    return midl::validate::admit(doc.as<JsonVariantConst>(), m.as<JsonVariantConst>(),
                                 midl_manifest::CLASS_ID, issue);
}

static void expect_reject(const char *json, const char *path) {
    static Issue issue;
    TEST_ASSERT_FALSE_MESSAGE(admit_json(json, issue), json);
    TEST_ASSERT_EQUAL_STRING(path, issue.path);
    TEST_ASSERT_TRUE(issue.message[0] != 0);
}

static void expect_ok(const char *json) {
    static Issue issue;
    bool ok = admit_json(json, issue);
    char msg[400];
    snprintf(msg, sizeof(msg), "%s: %s", issue.path, issue.message);
    TEST_ASSERT_TRUE_MESSAGE(ok, msg);
}

static std::string rep(char c, size_t n) {
    return std::string(n, c);
}

static std::string one_screen(const std::string &element_json, const char *id = "s") {
    return std::string(R"({"midl":"1.0.0","screens":[{"id":")") + id + R"(","elements":{"a":)" +
           element_json + R"(},"layout":{"element":"a"}}]})";
}

static const char *SV =
    R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"a.b"}}})";

// --- the old gate's gaps, now closed --------------------------------------

static void test_rejects_what_the_old_gate_accepted() {
    // Unknown element type (only had a `midl` marker and one screen).
    expect_reject(one_screen(R"({"type":"frobnicator"})").c_str(), "/screens/0/elements/a/type");
    // Missing required binding.
    expect_reject(one_screen(R"({"type":"gauge"})").c_str(),
                  "/screens/0/elements/a/bindings/value");
    // Layout references a missing element.
    expect_reject(
        R"({"midl":"1.0.0","screens":[{"id":"s","elements":{},"layout":{"element":"zz"}}]})",
        "/screens/0/layout/element");
    // Incompatible version.
    expect_reject(
        R"({"midl":"2.0.0","screens":[{"id":"s","elements":{"a":{"type":"button"}},"layout":{"element":"a"}}]})",
        "/midl");
    // Ten tiles on a 9-tile class.
    expect_reject(
        R"({"midl":"1.0.0","screens":[{"id":"s","elements":{"a":{"type":"button"}},"layout":{"flow":"row","children":[{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"},{"element":"a"}]}}]})",
        "/screens/0/layout");
    // Binding field the element does not advertise.
    expect_reject(
        one_screen(
            R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"a.b"},"dir":{"kind":"signalk","path":"c.d"}}})")
            .c_str(),
        "/screens/0/elements/a/bindings/dir");
    // Inverted numeric range.
    expect_reject(
        one_screen(
            R"({"type":"gauge","style":{"range":[100,0]},"bindings":{"value":{"kind":"signalk","path":"a.b"}}})")
            .c_str(),
        "/screens/0/elements/a/style/range");
    // Negative decimals (schema minimum 0).
    expect_reject(
        one_screen(
            R"({"type":"single-value","format":{"decimals":-1},"bindings":{"value":{"kind":"signalk","path":"a.b"}}})")
            .c_str(),
        "/screens/0/elements/a/format/decimals");
    // A legacy kind-less binding is no longer admissible (the schema requires kind).
    expect_reject(
        one_screen(R"({"type":"single-value","bindings":{"value":{"path":"a.b"}}})").c_str(),
        "/screens/0/elements/a/bindings/value");
    expect_ok(one_screen(SV).c_str());
}

// --- stage 5: firmware storage limits --------------------------------------

static std::string n_screens(size_t n) {
    std::string s = R"({"midl":"1.0.0","screens":[)";
    for (size_t i = 0; i < n; ++i) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 R"(%s{"id":"s%u","elements":{"a":{"type":"button"}},"layout":{"element":"a"}})",
                 i ? "," : "", (unsigned)i);
        s += buf;
    }
    return s + "]}";
}

static void test_screen_count_limit() {
    expect_ok(n_screens(midl::validate::MAX_SCREENS).c_str());
    expect_reject(n_screens(midl::validate::MAX_SCREENS + 1).c_str(), "/screens/16");
}

static void test_string_length_limits() {
    // Screen id (nav/zoom targets compare against it): 31 ok, 32 rejected.
    expect_ok(one_screen(SV, rep('i', 31).c_str()).c_str());
    expect_reject(one_screen(SV, rep('i', 32).c_str()).c_str(), "/screens/0/id");
    // Element name / unit.
    expect_ok(one_screen(R"({"type":"button","name":")" + rep('n', 31) + R"("})").c_str());
    expect_reject(one_screen(R"({"type":"button","name":")" + rep('n', 32) + R"("})").c_str(),
                  "/screens/0/elements/a/name");
    expect_reject(one_screen(R"({"type":"single-value","format":{"unit":")" + rep('u', 32) +
                             R"("},"bindings":{"value":{"kind":"signalk","path":"a.b"}}})")
                      .c_str(),
                  "/screens/0/elements/a/format/unit");
    // Element id (the elements map key).
    {
        std::string doc = R"({"midl":"1.0.0","screens":[{"id":"s","elements":{")" + rep('e', 32) +
                          R"(":{"type":"button"}},"layout":{"element":")" + rep('e', 32) +
                          R"("}}]})";
        expect_reject(doc.c_str(), ("/screens/0/elements/" + rep('e', 32)).c_str());
    }
    // SignalK path: 95 ok, 96 rejected (a truncated path subscribes a different one).
    expect_ok(
        one_screen(R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"a.)" +
                   rep('b', 93) + R"("}}})")
            .c_str());
    expect_reject(
        one_screen(R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"a.)" +
                   rep('b', 94) + R"("}}})")
            .c_str(),
        "/screens/0/elements/a/bindings/value/path");
    // Local metric id.
    expect_reject(
        one_screen(R"({"type":"single-value","bindings":{"value":{"kind":"local","id":")" +
                   rep('l', 96) + R"("}}})")
            .c_str(),
        "/screens/0/elements/a/bindings/value/id");
    // Nav target.
    expect_reject(one_screen(R"({"type":"button","action":{"kind":"nav","target":")" +
                             rep('t', 96) + R"("}})")
                      .c_str(),
                  "/screens/0/elements/a/action/target");
    // Command target (the renderer drops a long one; midl#25 rule).
    expect_reject(one_screen(R"({"type":"button","action":{"kind":"command","target":")" +
                             rep('t', 96) + R"("}})")
                      .c_str(),
                  "/screens/0/elements/a/action/target");
    // PUT string payload: 29 fits "\"%s\"" in 32 bytes, 30 would be truncated.
    expect_ok(one_screen(R"({"type":"button","action":{"kind":"put","target":"a.b","value":")" +
                         rep('v', 29) + R"("}})")
                  .c_str());
    expect_reject(one_screen(R"({"type":"button","action":{"kind":"put","target":"a.b","value":")" +
                             rep('v', 30) + R"("}})")
                      .c_str(),
                  "/screens/0/elements/a/action/value");
}

static void test_grid_slot_limit() {
    // 8x8 = 64 slots is the solver's limit; 9x8 is refused by the solver, so
    // reject it up front.
    expect_ok(
        R"({"midl":"1.0.0","screens":[{"id":"s","elements":{"a":{"type":"button"}},"layout":{"rows":8,"cols":8,"cells":[{"element":"a","colSpan":8,"rowSpan":8}]}}]})");
    expect_reject(
        R"({"midl":"1.0.0","screens":[{"id":"s","elements":{"a":{"type":"button"}},"layout":{"rows":9,"cols":8,"cells":[{"element":"a","colSpan":8,"rowSpan":9}]}}]})",
        "/screens/0/layout");
}

static void test_demo_doc_is_admissible() {
    expect_ok(midl::demo::SQUARE_480_JSON);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_corpus_agrees_with_midl_validators);
    RUN_TEST(test_embedded_manifest_agrees_on_its_class);
    RUN_TEST(test_rejects_what_the_old_gate_accepted);
    RUN_TEST(test_screen_count_limit);
    RUN_TEST(test_string_length_limits);
    RUN_TEST(test_grid_slot_limit);
    RUN_TEST(test_demo_doc_is_admissible);
    return UNITY_END();
}
