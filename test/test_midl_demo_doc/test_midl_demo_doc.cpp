// instruments#19 review: the baked factory doc (midl::demo::SQUARE_480_JSON) is
// rendered by every board, whatever its MIDL display class. Every screen must
// solve within EVERY class's limits (round-360 is the tightest: depth 2 / 4
// tiles), placing each of its elements exactly once.
#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include "midl_demo_doc.h"
#include "midl_solve.h"

// Each class's generated limits, from the same committed headers the device
// builds use (wrapped so the identical midl_gen:: names do not collide). The
// host build's own class (square-480) is already included via midl_solve.h, so
// it is the global midl_gen:: (asserted below).
namespace cls_square_480 {
namespace midl_gen = ::midl_gen;
}
namespace cls_landscape_800x480 {
#include "midl_gen/landscape-800x480/limits.h"
}
namespace cls_landscape_1024x600 {
#include "midl_gen/landscape-1024x600/limits.h"
}
namespace cls_round_360 {
#include "midl_gen/round-360/limits.h"
}

struct ClassLimits {
    const char *id;
    int w, h, max_tiles, max_depth;
};

#define CLS(ns, W, H)                                                                              \
    { ns::midl_gen::CLASS_ID, W, H, ns::midl_gen::MAX_TILES, ns::midl_gen::MAX_DEPTH }
static const ClassLimits kClasses[] = {
    CLS(cls_square_480, 480, 480),
    CLS(cls_landscape_800x480, 800, 480),
    CLS(cls_landscape_1024x600, 1024, 600),
    CLS(cls_round_360, 360, 360),
};

void setUp() {
}
void tearDown() {
}

static void test_baked_doc_solves_for_every_class() {
    TEST_ASSERT_EQUAL_STRING("square-480", ::midl_gen::CLASS_ID);  // host default class
    JsonDocument doc;
    TEST_ASSERT_EQUAL(DeserializationError::Ok,
                      deserializeJson(doc, midl::demo::SQUARE_480_JSON).code());
    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(6, (int)screens.size());

    for (const ClassLimits &c : kClasses) {
        for (JsonObjectConst s : screens) {
            char msg[128];
            snprintf(msg, sizeof(msg), "class %s screen %s", c.id, s["id"] | "?");
            midl::PlacementSet out;
            midl::SolveStatus st = midl::solve_screen(s["layout"], midl::Rect{0, 0, c.w, c.h}, out,
                                                      c.max_depth, (size_t)c.max_tiles);
            TEST_ASSERT_EQUAL_INT_MESSAGE(midl::SOLVE_OK, st, msg);
            TEST_ASSERT_TRUE_MESSAGE(out.count <= (size_t)c.max_tiles, msg);

            // Content kept: every element placed exactly once, nothing unknown.
            JsonObjectConst els = s["elements"].as<JsonObjectConst>();
            TEST_ASSERT_EQUAL_size_t_MESSAGE(els.size(), out.count, msg);
            for (size_t i = 0; i < out.count; i++) {
                TEST_ASSERT_TRUE_MESSAGE(els[out.items[i].element].is<JsonObjectConst>(), msg);
                for (size_t j = i + 1; j < out.count; j++)
                    TEST_ASSERT_TRUE_MESSAGE(
                        strcmp(out.items[i].element, out.items[j].element) != 0, msg);
            }
        }
    }
}

// The round-safe rewrite must not move anything on square-480: the hero-split
// preset is geometrically identical to the library's row[a | col[b, c]].
static void test_hero_split_matches_nested_split_geometry() {
    JsonDocument nested, preset;
    deserializeJson(
        nested,
        R"({"flow":"row","children":[{"element":"a"},{"flow":"col","children":[{"element":"b"},{"element":"c"}]}]})");
    deserializeJson(preset, R"({"preset":"hero-split","slots":["a","b","c"]})");
    midl::PlacementSet pn, pp;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_OK,
                          midl::solve_screen(nested.as<JsonVariantConst>(), {0, 0, 480, 480}, pn));
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_OK,
                          midl::solve_screen(preset.as<JsonVariantConst>(), {0, 0, 480, 480}, pp));
    TEST_ASSERT_EQUAL_size_t(3, pn.count);
    TEST_ASSERT_EQUAL_size_t(3, pp.count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_STRING(pn.items[i].element, pp.items[i].element);
        TEST_ASSERT_EQUAL_MEMORY(&pn.items[i].rect, &pp.items[i].rect, sizeof(midl::Rect));
    }
}

// The explicit-limits overload is what catches the round regression.
static void test_explicit_limits_reject_depth3_on_round() {
    JsonDocument nested;
    deserializeJson(
        nested,
        R"({"flow":"row","children":[{"element":"a"},{"flow":"col","children":[{"element":"b"},{"element":"c"}]}]})");
    midl::PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_DEEP,
                          midl::solve_screen(nested.as<JsonVariantConst>(), {0, 0, 360, 360}, out,
                                             cls_round_360::midl_gen::MAX_DEPTH,
                                             (size_t)cls_round_360::midl_gen::MAX_TILES));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_explicit_limits_reject_depth3_on_round);
    RUN_TEST(test_hero_split_matches_nested_split_geometry);
    RUN_TEST(test_baked_doc_solves_for_every_class);
    return UNITY_END();
}
