// Host guard: every shipped firmware-projection fixture must fit the
// firmware POD bounds, and the embedded manifest must never advertise a
// square-480 limit larger than the firmware can hold. Pure host test —
// no device, no LVGL.

#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string>

// midl_limits.h already pulls in layout.h transitively; no need to include
// layout.h directly.
#include "midl_limits.h"
#include "generated_midl_manifest.h"  // midl_manifest::JSON, embedded square-480
// layout.h is already in scope via midl_limits.h → layout.h

// Counts leaves and measures max depth of a node tree. `dir`/`flow` split,
// `grid` cells, `preset` (counts slots as leaves), or a leaf `{element}`.
static void walk(JsonVariantConst node, int depth, size_t &leaves, int &maxDepth) {
    if (depth > maxDepth) maxDepth = depth;
    if (node.containsKey("element")) {
        leaves += 1;
        return;
    }
    if (node.containsKey("preset")) {
        leaves += node["slots"].as<JsonArrayConst>().size();
        return;
    }
    JsonArrayConst kids = node.containsKey("children") ? node["children"].as<JsonArrayConst>()
                                                       : node["cells"].as<JsonArrayConst>();
    for (JsonVariantConst k : kids)
        walk(k, depth + 1, leaves, maxDepth);
}

static std::string slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::string s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

// Fixture paths are relative to the project root, which is the cwd that
// PlatformIO's native runner (and `make test`) sets before executing the binary.
static void check_fixture(const char *path) {
    std::string json = slurp(path);
    JsonDocument doc;
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, json), path);

    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    TEST_ASSERT_LESS_OR_EQUAL_size_t(midl::FirmwareLimits::max_screens, screens.size());

    for (JsonVariantConst scr : screens) {
        size_t leaves = 0;
        int maxDepth = 0;
        walk(scr["layout"], 1, leaves, maxDepth);
        TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(midl::FirmwareLimits::max_tiles_per_screen, leaves,
                                                 path);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE((int)midl::FirmwareLimits::max_depth, maxDepth, path);
    }
}

void test_full_1tile_fits() {
    check_fixture("test/fixtures/yb-midl/projection/full-1tile.square-480.json");
}
void test_hero_split_fits() {
    check_fixture("test/fixtures/yb-midl/projection/hero-split-3tile.square-480.json");
}
void test_steering_4tile_fits() {
    check_fixture("test/fixtures/yb-midl/projection/steering-4tile.square-480.json");
}

void test_manifest_within_firmware_limits() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, midl_manifest::JSON));
    bool sawSquare = false;
    for (JsonVariantConst cls : doc["classes"].as<JsonArrayConst>()) {
        if (std::string(cls["id"] | "") != "square-480") continue;
        sawSquare = true;
        // The device must be able to HOLD whatever the manifest advertises.
        TEST_ASSERT_LESS_OR_EQUAL_size_t(midl::FirmwareLimits::max_tiles_per_screen,
                                         (size_t)(cls["maxTiles"] | 999));
        TEST_ASSERT_LESS_OR_EQUAL_INT(midl::FirmwareLimits::max_depth, cls["maxDepth"] | 999);
    }
    TEST_ASSERT_TRUE_MESSAGE(sawSquare, "square-480 class missing from embedded manifest");
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_full_1tile_fits);
    RUN_TEST(test_hero_split_fits);
    RUN_TEST(test_steering_4tile_fits);
    RUN_TEST(test_manifest_within_firmware_limits);
    return UNITY_END();
}
