#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <string>

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
    const midl::Placement *result = nullptr;
    for (size_t i = 0; i < p.count; i++) {
        if (strcmp(p.items[i].element, id) == 0) {
            TEST_ASSERT_NULL_MESSAGE(result, "duplicate element id in PlacementSet");
            result = &p.items[i];
        }
    }
    return result;
}

void test_leaf_fills_area() {
    PlacementSet p = solve(R"({"element":"a"})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);    // NOLINT — guarded above
    TEST_ASSERT_EQUAL_INT(480, a->rect.w);  // NOLINT
    TEST_ASSERT_EQUAL_INT(480, a->rect.h);  // NOLINT
}

void test_row_split_equal() {
    PlacementSet p =
        solve(R"({"flow":"row","children":[{"element":"a"},{"element":"b"}]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(2, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    const midl::Placement *b = find(p, "b");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(240, a->rect.w);
    TEST_ASSERT_EQUAL_INT(240, b->rect.x);
    TEST_ASSERT_EQUAL_INT(240, b->rect.w);
}

void test_row_split_weighted_distributes_remainder() {
    // 481 px, weights 1:2 -> boundaries at floor(481*1/3)=160, then 481.
    // a = [0,160), b = [160,481): widths 160 and 321, summing to 481 exactly.
    PlacementSet p =
        solve(R"({"flow":"row","weights":[1,2],"children":[{"element":"a"},{"element":"b"}]})",
              {0, 0, 481, 480});
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    const midl::Placement *b = find(p, "b");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(160, a->rect.w);
    TEST_ASSERT_EQUAL_INT(160, b->rect.x);
    TEST_ASSERT_EQUAL_INT(321, b->rect.w);
}

void test_col_split_nested() {
    PlacementSet p = solve(
        R"({"flow":"col","children":[{"element":"a"},{"flow":"row","children":[{"element":"b"},{"element":"c"}]}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    const midl::Placement *b = find(p, "b");
    TEST_ASSERT_NOT_NULL(b);
    const midl::Placement *c = find(p, "c");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, a->rect.y);
    TEST_ASSERT_EQUAL_INT(240, a->rect.h);
    TEST_ASSERT_EQUAL_INT(240, b->rect.y);
    TEST_ASSERT_EQUAL_INT(240, b->rect.w);  // half of 480 width
    TEST_ASSERT_EQUAL_INT(240, c->rect.x);
}

void test_grid_2x2_rowmajor() {
    PlacementSet p = solve(
        R"({"rows":2,"cols":2,"cells":[{"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(4, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    const midl::Placement *b = find(p, "b");
    TEST_ASSERT_NOT_NULL(b);
    const midl::Placement *c = find(p, "c");
    TEST_ASSERT_NOT_NULL(c);
    const midl::Placement *d = find(p, "d");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(0, a->rect.y);
    TEST_ASSERT_EQUAL_INT(240, b->rect.x);
    TEST_ASSERT_EQUAL_INT(0, b->rect.y);
    TEST_ASSERT_EQUAL_INT(0, c->rect.x);
    TEST_ASSERT_EQUAL_INT(240, c->rect.y);
    TEST_ASSERT_EQUAL_INT(240, d->rect.x);
    TEST_ASSERT_EQUAL_INT(240, d->rect.y);
    TEST_ASSERT_EQUAL_INT(240, a->rect.w);
    TEST_ASSERT_EQUAL_INT(240, a->rect.h);
}

void test_preset_full() {
    PlacementSet p = solve(R"({"preset":"full","slots":["a"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(1, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(480, a->rect.w);
}

void test_preset_hero_split() {
    // {1,{2,3}} = row[ leaf, col[leaf,leaf] ], equal weights.
    PlacementSet p = solve(R"({"preset":"hero-split","slots":["hero","x","y"]})", {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(3, p.count);
    const midl::Placement *hero = find(p, "hero");
    TEST_ASSERT_NOT_NULL(hero);
    const midl::Placement *x = find(p, "x");
    TEST_ASSERT_NOT_NULL(x);
    const midl::Placement *y = find(p, "y");
    TEST_ASSERT_NOT_NULL(y);
    TEST_ASSERT_EQUAL_INT(0, hero->rect.x);
    TEST_ASSERT_EQUAL_INT(240, hero->rect.w);
    TEST_ASSERT_EQUAL_INT(240, x->rect.x);
    TEST_ASSERT_EQUAL_INT(0, x->rect.y);
    TEST_ASSERT_EQUAL_INT(240, y->rect.y);
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
    // 10 leaves in a row > max_tiles_per_screen (spec-derived; square-480 = 9).
    JsonDocument doc;
    deserializeJson(doc,
                    R"({"flow":"row","children":[
        {"element":"a"},{"element":"b"},{"element":"c"},{"element":"d"},{"element":"e"},
        {"element":"f"},{"element":"g"},{"element":"h"},{"element":"i"},{"element":"j"}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_TOO_MANY_TILES,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

void test_grid_3x3_rowmajor() {
    // 9-tile square-480 grid: 160 px cells, row-major.
    PlacementSet p = solve(
        R"({"rows":3,"cols":3,"cells":[
            {"element":"a"},{"element":"b"},{"element":"c"},
            {"element":"d"},{"element":"e"},{"element":"f"},
            {"element":"g"},{"element":"h"},{"element":"i"}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(9, p.count);
    const midl::Placement *a = find(p, "a");
    TEST_ASSERT_NOT_NULL(a);
    const midl::Placement *e = find(p, "e");  // center
    TEST_ASSERT_NOT_NULL(e);
    const midl::Placement *i = find(p, "i");  // bottom-right
    TEST_ASSERT_NOT_NULL(i);
    TEST_ASSERT_EQUAL_INT(0, a->rect.x);
    TEST_ASSERT_EQUAL_INT(0, a->rect.y);
    TEST_ASSERT_EQUAL_INT(160, a->rect.w);
    TEST_ASSERT_EQUAL_INT(160, a->rect.h);
    TEST_ASSERT_EQUAL_INT(160, e->rect.x);
    TEST_ASSERT_EQUAL_INT(160, e->rect.y);
    TEST_ASSERT_EQUAL_INT(320, i->rect.x);
    TEST_ASSERT_EQUAL_INT(320, i->rect.y);
}

void test_grid_over_button_row_8tiles() {
    // Steering shape: col-split [grid 2x2 (weight 3), row of 4 buttons (weight 1)].
    // 8 leaves <= 9, depth 3 <= 3. Validates the >4-tile split layout.
    PlacementSet p = solve(
        R"({"flow":"col","weights":[3,1],"children":[
            {"rows":2,"cols":2,"cells":[
                {"element":"hdg"},{"element":"rud"},{"element":"xte"},{"element":"vmg"}]},
            {"flow":"row","children":[
                {"element":"n10"},{"element":"n1"},{"element":"p1"},{"element":"p10"}]}]})",
        {0, 0, 480, 480});
    TEST_ASSERT_EQUAL_size_t(8, p.count);
    const midl::Placement *hdg = find(p, "hdg");
    TEST_ASSERT_NOT_NULL(hdg);
    const midl::Placement *n10 = find(p, "n10");
    TEST_ASSERT_NOT_NULL(n10);
    // Top region = 3/4 of 480 = 360; grid cell = 180 tall. Button row = bottom 120.
    TEST_ASSERT_EQUAL_INT(0, hdg->rect.y);
    TEST_ASSERT_EQUAL_INT(180, hdg->rect.h);
    TEST_ASSERT_EQUAL_INT(360, n10->rect.y);
    TEST_ASSERT_EQUAL_INT(120, n10->rect.h);
    TEST_ASSERT_EQUAL_INT(120, n10->rect.w);  // 480 / 4 buttons
}

void test_malformed_node_rejected() {
    JsonDocument doc;
    deserializeJson(doc, R"({"frobnicate":true})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_BAD_NODE,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

void test_weights_length_mismatch_rejected() {
    // weights array has 1 entry but children has 2 — must be rejected.
    JsonDocument doc;
    deserializeJson(doc,
                    R"({"flow":"row","weights":[1],"children":[{"element":"a"},{"element":"b"}]})");
    PlacementSet out;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_BAD_NODE,
                          midl::solve_screen(doc.as<JsonVariantConst>(), {0, 0, 480, 480}, out));
}

static std::string slurp2(const char *path) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::string s;
    char b[1024];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0)
        s.append(b, n);
    fclose(f);
    return s;
}

void test_steering_fixture_geometry() {
    std::string j = slurp2("test/fixtures/yb-midl/projection/steering-4tile.square-480.json");
    JsonDocument doc;
    deserializeJson(doc, j);
    JsonVariantConst layout = doc["screens"][0]["layout"];
    PlacementSet p;
    TEST_ASSERT_EQUAL_INT(midl::SOLVE_OK, midl::solve_screen(layout, {0, 0, 480, 480}, p));
    TEST_ASSERT_EQUAL_size_t(4, p.count);
    const midl::Placement *compass = find(p, "compass");
    TEST_ASSERT_NOT_NULL(compass);
    const midl::Placement *cts = find(p, "cts");
    TEST_ASSERT_NOT_NULL(cts);
    const midl::Placement *xte = find(p, "xte");
    TEST_ASSERT_NOT_NULL(xte);
    const midl::Placement *rudder = find(p, "rudder");
    TEST_ASSERT_NOT_NULL(rudder);
    // row weights [3,2] over 480: compass = [0,288), right col = [288,480).
    TEST_ASSERT_EQUAL_INT(0, compass->rect.x);
    TEST_ASSERT_EQUAL_INT(288, compass->rect.w);
    TEST_ASSERT_EQUAL_INT(288, cts->rect.x);
    TEST_ASSERT_EQUAL_INT(192, cts->rect.w);
    // right column split into 3 equal rows over 480: 160 each.
    TEST_ASSERT_EQUAL_INT(0, cts->rect.y);
    TEST_ASSERT_EQUAL_INT(160, xte->rect.y);
    TEST_ASSERT_EQUAL_INT(320, rudder->rect.y);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_leaf_fills_area);
    RUN_TEST(test_row_split_equal);
    RUN_TEST(test_row_split_weighted_distributes_remainder);
    RUN_TEST(test_col_split_nested);
    RUN_TEST(test_grid_2x2_rowmajor);
    RUN_TEST(test_grid_3x3_rowmajor);
    RUN_TEST(test_grid_over_button_row_8tiles);
    RUN_TEST(test_preset_full);
    RUN_TEST(test_preset_hero_split);
    RUN_TEST(test_preset_unknown_rejected);
    RUN_TEST(test_too_deep_rejected);
    RUN_TEST(test_too_many_tiles_rejected);
    RUN_TEST(test_malformed_node_rejected);
    RUN_TEST(test_weights_length_mismatch_rejected);
    RUN_TEST(test_steering_fixture_geometry);
    return UNITY_END();
}
