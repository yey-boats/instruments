#include "capabilities.h"

#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "marker_math.h"

void setUp() {
}
void tearDown() {
}

static void test_version_and_limits() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    TEST_ASSERT_EQUAL_INT(capabilities::MANIFEST_VERSION, doc["version"].as<int>());
    TEST_ASSERT_EQUAL_INT(8, doc["maxViews"].as<int>());
    TEST_ASSERT_EQUAL_INT(4, doc["maxTilesPerScreen"].as<int>());
    TEST_ASSERT_EQUAL_STRING("open", doc["paths"].as<const char *>());
}

static void test_view_types_present() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    JsonObject vts = doc["viewTypes"];
    for (const char *t :
         {"numeric", "compass", "windCircle", "gauge", "bar", "trend", "text", "control"}) {
        TEST_ASSERT_TRUE_MESSAGE(vts[t].is<JsonObject>(), t);
    }
    // numeric: single value path + the standard attrs incl. unit + color.
    JsonArray np = vts["numeric"]["paths"];
    TEST_ASSERT_EQUAL_INT(1, np.size());
    TEST_ASSERT_EQUAL_STRING("value", np[0].as<const char *>());
    // windCircle binds value + dir.
    JsonArray wp = vts["windCircle"]["paths"];
    TEST_ASSERT_EQUAL_INT(2, wp.size());
    TEST_ASSERT_EQUAL_STRING("dir", wp[1].as<const char *>());
}

static void test_gauge_has_range_and_zones() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    JsonArray attrs = doc["viewTypes"]["gauge"]["attrs"];
    bool has_range = false, has_zones = false;
    for (JsonVariant a : attrs) {
        if (!strcmp(a.as<const char *>(), "range")) has_range = true;
        if (!strcmp(a.as<const char *>(), "zones")) has_zones = true;
    }
    TEST_ASSERT_TRUE(has_range);
    TEST_ASSERT_TRUE(has_zones);
}

static void test_font_sizes_and_units() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    JsonArray fs = doc["fontSizes"];
    TEST_ASSERT_TRUE(fs.size() >= 2);
    TEST_ASSERT_EQUAL_INT(14, fs[0].as<int>());  // DEFAULT_SIZES[0]
    // speed unit family includes kn + m/s.
    JsonArray speed = doc["units"]["speed"];
    TEST_ASSERT_EQUAL_STRING("kn", speed[0].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("m/s", speed[1].as<const char *>());
}

static void test_controls_and_themes() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    TEST_ASSERT_EQUAL_STRING("autopilot", doc["controls"][0].as<const char *>());
    JsonArray themes = doc["themes"];
    TEST_ASSERT_EQUAL_INT(3, themes.size());
    TEST_ASSERT_EQUAL_STRING("day", themes[0].as<const char *>());
}

static void test_markers_glyphs_and_cap() {
    JsonDocument doc;
    capabilities::build_manifest(doc.to<JsonObject>());
    // Per-dial marker cap mirrors marker_math's single source of truth.
    TEST_ASSERT_EQUAL_INT((int)ui::kMaxMarkersPerDial, doc["maxMarkersPerDial"].as<int>());
    TEST_ASSERT_EQUAL_INT(12, doc["maxMarkersPerDial"].as<int>());
    // Glyph token set: one entry per GlyphId, in canonical order.
    TEST_ASSERT_TRUE_MESSAGE(doc["glyphs"].is<JsonArray>(), "glyphs is not an array");
    JsonArray glyphs = doc["glyphs"];
    TEST_ASSERT_EQUAL_INT((int)ui::GlyphId::COUNT, (int)glyphs.size());
    TEST_ASSERT_EQUAL_INT(10, (int)glyphs.size());
    bool has_triangle = false, has_diamond = false, has_chevron_in = false,
         has_chevron_double = false;
    for (JsonVariant g : glyphs) {
        const char *t = g.as<const char *>();
        if (!strcmp(t, "triangle")) has_triangle = true;
        if (!strcmp(t, "diamond")) has_diamond = true;
        if (!strcmp(t, "chevron_in")) has_chevron_in = true;
        if (!strcmp(t, "chevron_double")) has_chevron_double = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(has_triangle, "glyphs missing triangle");
    TEST_ASSERT_TRUE_MESSAGE(has_diamond, "glyphs missing diamond");
    TEST_ASSERT_TRUE_MESSAGE(has_chevron_in, "glyphs missing chevron_in");
    TEST_ASSERT_TRUE_MESSAGE(has_chevron_double, "glyphs missing chevron_double");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_version_and_limits);
    RUN_TEST(test_view_types_present);
    RUN_TEST(test_gauge_has_range_and_zones);
    RUN_TEST(test_font_sizes_and_units);
    RUN_TEST(test_controls_and_themes);
    RUN_TEST(test_markers_glyphs_and_cap);
    return UNITY_END();
}
