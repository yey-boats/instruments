#include "midl_solve.h"

#include <stdint.h>
#include <string.h>

namespace midl {

namespace {

// Integer boundary for fraction `acc/total` of `span`. Using cumulative
// weights and flooring each boundary distributes the remainder deterministically
// and makes the last child end exactly at `span` (no gaps, no overlap).
inline int boundary(int span, int64_t acc, int64_t total) {
    return (int)((int64_t)span * acc / total);
}

bool push_leaf(const char *id, Rect r, PlacementSet &out) {
    if (out.count >= FirmwareLimits::max_tiles_per_screen) return false;
    Placement &p = out.items[out.count++];
    memset(&p, 0, sizeof(p));
    strncpy(p.element, id, sizeof(p.element) - 1);
    p.rect = r;
    return true;
}

SolveStatus solve_node(JsonVariantConst node, Rect area, int depth, PlacementSet &out);

SolveStatus solve_split(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    JsonArrayConst kids = node["children"].as<JsonArrayConst>();
    if (kids.isNull() || kids.size() == 0) return SOLVE_BAD_NODE;
    bool row = strcmp(node["flow"] | node["dir"] | "row", "row") == 0;

    JsonArrayConst weights = node["weights"].as<JsonArrayConst>();
    if (!weights.isNull() && weights.size() != kids.size()) return SOLVE_BAD_NODE;
    int64_t total = 0;
    for (size_t i = 0; i < kids.size(); i++)
        total += weights.isNull() ? 1 : (int64_t)(weights[i] | 1);
    if (total <= 0) return SOLVE_BAD_NODE;

    int64_t acc = 0;
    int span = row ? area.w : area.h;
    int prev = 0;
    for (size_t i = 0; i < kids.size(); i++) {
        acc += weights.isNull() ? 1 : (int64_t)(weights[i] | 1);
        int next = boundary(span, acc, total);
        Rect cr = row ? Rect{area.x + prev, area.y, next - prev, area.h}
                      : Rect{area.x, area.y + prev, area.w, next - prev};
        prev = next;
        SolveStatus s = solve_node(kids[i], cr, depth + 1, out);
        if (s != SOLVE_OK) return s;
    }
    return SOLVE_OK;
}

SolveStatus solve_grid(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    int rows = node["rows"] | 0, cols = node["cols"] | 0;
    JsonArrayConst cells = node["cells"].as<JsonArrayConst>();
    if (rows <= 0 || cols <= 0 || cells.isNull()) return SOLVE_BAD_NODE;
    if ((int)cells.size() != rows * cols) return SOLVE_BAD_NODE;  // row-major, full
    for (int i = 0; i < (int)cells.size(); i++) {
        int r = i / cols, c = i % cols;
        int x0 = area.x + boundary(area.w, c, cols), x1 = area.x + boundary(area.w, c + 1, cols);
        int y0 = area.y + boundary(area.h, r, rows), y1 = area.y + boundary(area.h, r + 1, rows);
        SolveStatus s = solve_node(cells[i], Rect{x0, y0, x1 - x0, y1 - y0}, depth + 1, out);
        if (s != SOLVE_OK) return s;
    }
    return SOLVE_OK;
}

SolveStatus solve_node(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    if (depth > FirmwareLimits::max_depth) return SOLVE_TOO_DEEP;
    if (node["element"].is<const char *>())
        return push_leaf(node["element"] | "", area, out) ? SOLVE_OK : SOLVE_TOO_MANY_TILES;
    if (node["preset"].is<const char *>()) return expand_preset(node, area, out);
    if (node["children"].is<JsonArrayConst>()) return solve_split(node, area, depth, out);
    if (node["cells"].is<JsonArrayConst>()) return solve_grid(node, area, depth, out);
    return SOLVE_BAD_NODE;
}

}  // namespace

SolveStatus expand_preset(JsonVariantConst node, Rect area, PlacementSet &out) {
    const char *name = node["preset"] | "";
    JsonArrayConst slots = node["slots"].as<JsonArrayConst>();
    if (slots.isNull()) return SOLVE_BAD_NODE;

    if (strcmp(name, "full") == 0) {
        if (slots.size() < 1) return SOLVE_BAD_NODE;
        return push_leaf(slots[0] | "", area, out) ? SOLVE_OK : SOLVE_TOO_MANY_TILES;
    }
    if (strcmp(name, "hero-split") == 0) {
        if (slots.size() != 3) return SOLVE_BAD_NODE;
        // row[ hero(1) | col[ s1, s2 ] ] with equal weights -> hero gets left half.
        int mid = boundary(area.w, 1, 2);
        Rect left{area.x, area.y, mid, area.h};
        Rect right{area.x + mid, area.y, area.w - mid, area.h};
        int rmid = boundary(right.h, 1, 2);
        if (!push_leaf(slots[0] | "", left, out)) return SOLVE_TOO_MANY_TILES;
        if (!push_leaf(slots[1] | "", Rect{right.x, right.y, right.w, rmid}, out))
            return SOLVE_TOO_MANY_TILES;
        if (!push_leaf(slots[2] | "", Rect{right.x, right.y + rmid, right.w, right.h - rmid}, out))
            return SOLVE_TOO_MANY_TILES;
        return SOLVE_OK;
    }
    return SOLVE_UNKNOWN_PRESET;
}

SolveStatus solve_screen(JsonVariantConst layout, Rect area, PlacementSet &out) {
    out.count = 0;
    return solve_node(layout, area, 1, out);
}

}  // namespace midl
