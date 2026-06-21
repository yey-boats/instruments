#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>

#include "midl_solve.h"

using midl::PlacementSet;
using midl::Rect;

static PlacementSet solve(const char *json, Rect area) {
    JsonDocument doc;
    deserializeJson(doc, json);
    PlacementSet out;
    midl::solve_screen(doc.as<JsonVariantConst>(), area, out);
    return out;
}

static const midl::Placement *find(const PlacementSet &p, const char *id) {
    for (size_t i = 0; i < p.count; i++)
        if (strcmp(p.items[i].element, id) == 0) return &p.items[i];
    return nullptr;
}

void test_leaf_fills_area() {
    PlacementSet p = solve(R"({"element":"a"})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(480, a->rect.w);
    TEST_ASSERT_EQUAL_INT(480, a->rect.h);
}

void test_row_split_equal() {
    PlacementSet p =
        solve(R"({"flow":"row","children":[{"element":"a"},{"element":"b"}]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.w);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.w);
}

void test_row_split_weighted_distributes_remainder() {
    // 481 px, weights 1:2 -> boundaries at floor(481*1/3)=160, then 481.
    // a = [0,160), b = [160,481): widths 160 and 321, summing to 481 exactly.
    PlacementSet p =
        solve(R"({"flow":"row","weights":[1,2],"children":[{"element":"a"},{"element":"b"}]})",
              {0, 0, 481, 480});
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.x);
    TEST_ASSERT_EQUAL_INT(160, find(p, "a")->rect.w);
    TEST_ASSERT_EQUAL_INT(160, find(p, "b")->rect.x);
    TEST_ASSERT_EQUAL_INT(321, find(p, "b")->rect.w);
}

void test_col_split_nested() {
    PlacementSet p = solve(
        R"({"flow":"col","children":[{"element":"a"},{"flow":"row","children":[{"element":"b"},{"element":"c"}]}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.h);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.w);  // half of 480 width
    TEST_ASSERT_EQUAL_INT(240, find(p, "c")->rect.x);
}

void test_grid_2x2_rowmajor() {
    PlacementSet p = solve(
        R"({"rows":2,"cols":2,"cells":[{"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(4, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.x);
    TEST_ASSERT_EQUAL_INT(0, find(p, "a")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "b")->rect.x);
    TEST_ASSERT_EQUAL_INT(0, find(p, "b")->rect.y);
    TEST_ASSERT_EQUAL_INT(0, find(p, "c")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "c")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "d")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "d")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.w);
    TEST_ASSERT_EQUAL_INT(240, find(p, "a")->rect.h);
}

void test_preset_full() {
    PlacementSet p = solve(R"({"preset":"full","slots":["a"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    TEST_ASSERT_EQUAL_INT(480, find(p, "a")->rect.w);
}

void test_preset_hero_split() {
    // {1,{2,3}} = row[ leaf, col[leaf,leaf] ], equal weights.
    PlacementSet p = solve(R"({"preset":"hero-split","slots":["hero","x","y"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    TEST_ASSERT_EQUAL_INT(0, find(p, "hero")->rect.x);
    TEST_ASSERT_EQUAL_INT(240, find(p, "hero")->rect.w);
    TEST_ASSERT_EQUAL_INT(240, find(p, "x")->rect.x);
    TEST_ASSERT_EQUAL_INT(0, find(p, "x")->rect.y);
    TEST_ASSERT_EQUAL_INT(240, find(p, "y")->rect.y);
}

void test_preset_unknown_rejected() {
    JsonDocument doc;
    deserializeJson(doc, R"({"preset":"nope","slots":["a"]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_UNKNOWN_PRESET,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

void test_too_deep_rejected() {
    // depth 4 > MAX_LAYOUT_DEPTH (3): row>col>row>leaf
    JsonDocument doc;
    deserializeJson(doc,
                    R"({"flow":"row","children":[{"flow":"col","children":[
        {"flow":"row","children":[{"element":"a"}]}]}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_DEEP,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

void test_too_many_tiles_rejected() {
    // 5 leaves in a row > max_tiles_per_screen (4)
    JsonDocument doc;
    deserializeJson(doc,
                    R"({"flow":"row","children":[
        {"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"},{"element":"e"}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_MANY_TILES,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

void test_malformed_node_rejected() {
    JsonDocument doc;
    deserializeJson(doc, R"({"frobnicate":true})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_BAD_NODE,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_leaf_fills_area);
    RUN_TEST(test_row_split_equal);
    RUN_TEST(test_row_split_weighted_distributes_remainder);
    RUN_TEST(test_col_split_nested);
    RUN_TEST(test_grid_2x2_rowmajor);
    RUN_TEST(test_preset_full);
    RUN_TEST(test_preset_hero_split);
    RUN_TEST(test_preset_unknown_rejected);
    RUN_TEST(test_too_deep_rejected);
    RUN_TEST(test_too_many_tiles_rejected);
    RUN_TEST(test_malformed_node_rejected);
    return UNITY_END();
}
