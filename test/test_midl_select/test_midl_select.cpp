// Host tests for the pure MIDL selection helpers select_screen / find_element
// (src/midl_render.cpp). These are the host-clean extractions of the screen-
// array scan and element lookup that apply_doc (device-only) now calls.
//
// Regression coverage for two real bugs (fixed in cd8f294):
//   bug 1 — `screens` is a JSON ARRAY; an `is<JsonObjectConst>()` check on the
//           whole `screens` node rejected the correct shape and rendered nothing.
//   bug 2 — element lookup via operator[] missed keys built in a separate buffer
//           (ArduinoJson pointer-identity fast path); must use explicit strcmp.
//
// Pure host test — no device, no LVGL.

#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "midl_render.h"

using midl::render::find_element;
using midl::render::select_screen;

// A two-screen MIDL doc with `screens` as a JSON ARRAY (the real shape).
static const char *DOC_TWO_SCREENS = R"({
  "screens": [
    {"id": "a", "title": "Alpha", "elements": {"wind": {"type": "single-value"}}},
    {"id": "b", "title": "Bravo", "elements": {"sog": {"type": "single-value"}}}
  ]
})";

// --- select_screen ---------------------------------------------------------

void test_select_matches_second_screen() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, DOC_TWO_SCREENS));
    const char *out_id = nullptr;
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "b", &out_id);
    TEST_ASSERT_TRUE(scr.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("b", scr["id"] | "");
    TEST_ASSERT_NOT_NULL(out_id);
    TEST_ASSERT_EQUAL_STRING("b", out_id);
}

void test_select_missing_id_falls_back_to_first() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, DOC_TWO_SCREENS));
    const char *out_id = nullptr;
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "z", &out_id);
    TEST_ASSERT_TRUE(scr.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("a", scr["id"] | "");
    TEST_ASSERT_NOT_NULL(out_id);
    TEST_ASSERT_EQUAL_STRING("a", out_id);
}

void test_select_null_id_returns_first() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, DOC_TWO_SCREENS));
    const char *out_id = nullptr;
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), nullptr, &out_id);
    TEST_ASSERT_TRUE(scr.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("a", scr["id"] | "");
    TEST_ASSERT_EQUAL_STRING("a", out_id);
}

void test_select_null_out_id_is_safe() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, DOC_TWO_SCREENS));
    // out_id == nullptr must not crash.
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "b", nullptr);
    TEST_ASSERT_TRUE(scr.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("b", scr["id"] | "");
}

// Regression for bug 1: `screens` is an ARRAY. The OLD apply_doc gated on
// doc["screens"].is<JsonObjectConst>(), which is FALSE for an array, so it
// bailed out and rendered nothing. select_screen must resolve the array form.
void test_select_array_form_resolves_non_null_regression() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, DOC_TWO_SCREENS));
    // Sanity: the node really is an array, not an object (the bug's premise).
    TEST_ASSERT_TRUE(doc["screens"].is<JsonArrayConst>());
    TEST_ASSERT_FALSE(doc["screens"].is<JsonObjectConst>());
    const char *out_id = nullptr;
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "a", &out_id);
    TEST_ASSERT_TRUE(scr.is<JsonObjectConst>());  // would have been null under the old check
    TEST_ASSERT_EQUAL_STRING("a", out_id);
}

void test_select_empty_array_returns_null() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, R"({"screens": []})"));
    const char *out_id = (const char *)0x1;  // must be cleared to null
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "a", &out_id);
    TEST_ASSERT_FALSE(scr.is<JsonObjectConst>());
    TEST_ASSERT_NULL(out_id);
}

void test_select_missing_screens_returns_null() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, R"({"title": "no screens here"})"));
    const char *out_id = (const char *)0x1;
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), "a", &out_id);
    TEST_ASSERT_FALSE(scr.is<JsonObjectConst>());
    TEST_ASSERT_NULL(out_id);
}

void test_select_no_object_screen_returns_null() {
    JsonDocument doc;
    // `screens` array exists but holds only non-object entries.
    TEST_ASSERT_FALSE(deserializeJson(doc, R"({"screens": [1, "two", null]})"));
    JsonVariantConst scr = select_screen(doc.as<JsonVariantConst>(), nullptr, nullptr);
    TEST_ASSERT_FALSE(scr.is<JsonObjectConst>());
}

// --- find_element ----------------------------------------------------------

void test_find_element_found() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(
        deserializeJson(doc, R"({"wind": {"type": "windrose"}, "sog": {"type": "single-value"}})"));
    JsonVariantConst el = find_element(doc.as<JsonVariantConst>(), "wind");
    TEST_ASSERT_TRUE(el.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("windrose", el["type"] | "");
}

void test_find_element_missing_returns_null() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, R"({"wind": {}, "sog": {}})"));
    JsonVariantConst el = find_element(doc.as<JsonVariantConst>(), "depth");
    TEST_ASSERT_FALSE(el.is<JsonObjectConst>());
    TEST_ASSERT_TRUE(el.isNull());
}

void test_find_element_not_an_object_returns_null() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, R"([1, 2, 3])"));
    JsonVariantConst el = find_element(doc.as<JsonVariantConst>(), "wind");
    TEST_ASSERT_TRUE(el.isNull());
}

void test_find_element_null_id_returns_null() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, R"({"wind": {}})"));
    JsonVariantConst el = find_element(doc.as<JsonVariantConst>(), nullptr);
    TEST_ASSERT_TRUE(el.isNull());
}

// Regression for bug 2: build the lookup key in a SEPARATE buffer (mimicking
// the solver's Placement.element buffer, which is NOT the same allocation as
// the JSON key). operator[] could miss it via pointer identity; find_element's
// explicit strcmp must still resolve it.
void test_find_element_separate_buffer_regression() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(
        deserializeJson(doc, R"({"wind": {"type": "windrose"}, "sog": {"type": "single-value"}})"));
    char key[32];
    memset(key, 0, sizeof(key));
    strncpy(key, "sog", sizeof(key) - 1);  // distinct storage from the JSON key
    JsonVariantConst el = find_element(doc.as<JsonVariantConst>(), key);
    TEST_ASSERT_TRUE(el.is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("single-value", el["type"] | "");
}

void setUp() {
}
void tearDown() {
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_select_matches_second_screen);
    RUN_TEST(test_select_missing_id_falls_back_to_first);
    RUN_TEST(test_select_null_id_returns_first);
    RUN_TEST(test_select_null_out_id_is_safe);
    RUN_TEST(test_select_array_form_resolves_non_null_regression);
    RUN_TEST(test_select_empty_array_returns_null);
    RUN_TEST(test_select_missing_screens_returns_null);
    RUN_TEST(test_select_no_object_screen_returns_null);
    RUN_TEST(test_find_element_found);
    RUN_TEST(test_find_element_missing_returns_null);
    RUN_TEST(test_find_element_not_an_object_returns_null);
    RUN_TEST(test_find_element_null_id_returns_null);
    RUN_TEST(test_find_element_separate_buffer_regression);
    return UNITY_END();
}
