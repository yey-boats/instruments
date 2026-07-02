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

// A grid SPACER cell: an object with none of element/preset/children/cells
// (midl types.ts Node union: `{ colSpan?, rowSpan? }`). It occupies its grid
// slots (spans honored) but emits no placement — ts/src/solve.ts yields [] for
// such nodes.
bool is_grid_spacer(JsonVariantConst node) {
    return !node["element"].is<const char *>() && !node["preset"].is<const char *>() &&
           !node["children"].is<JsonArrayConst>() && !node["cells"].is<JsonArrayConst>();
}

// Grid with colSpan/rowSpan + spacer cells. Occupancy tracking mirrors
// ts/src/solve.ts:44-64 exactly: a spanned cell marks all covered slots
// occupied; subsequent cells skip occupied slots; extra cells past the last
// free slot are ignored. When every span is 1 (or absent) this degenerates to
// the original row-major walk and produces byte-identical rects. Cell edges go
// through the shared boundary() so a 2-span cell ends exactly where the next
// 1-span column begins (integer remainder distribution, no gaps/overlap).
SolveStatus solve_grid(JsonVariantConst node, Rect area, int depth, PlacementSet &out) {
    int rows = node["rows"] | 0, cols = node["cols"] | 0;
    JsonArrayConst cells = node["cells"].as<JsonArrayConst>();
    if (rows <= 0 || cols <= 0 || cells.isNull()) return SOLVE_BAD_NODE;
    // Fixed-size occupancy map: 64 bytes per grid frame is safe on the 8 KB
    // solver stack even at max nesting depth. Larger grids are rejected (a
    // 64-slot grid already dwarfs max_tiles_per_screen).
    constexpr int kMaxSlots = 64;
    const int total_slots = rows * cols;
    if (total_slots > kMaxSlots) return SOLVE_BAD_NODE;
    bool occupied[kMaxSlots] = {};
    int slot = 0;  // next candidate slot, row-major
    for (JsonVariantConst child : cells) {
        while (slot < total_slots && occupied[slot])
            slot++;
        if (slot >= total_slots) break;  // more cells than free slots (TS parity)
        int r = slot / cols, c = slot % cols;
        // Optional spans, clamped to the remaining grid space (TS parity). A
        // missing/degenerate span is 1.
        int cs = child["colSpan"] | 1, rs = child["rowSpan"] | 1;
        if (cs < 1) cs = 1;
        if (rs < 1) rs = 1;
        if (cs > cols - c) cs = cols - c;
        if (rs > rows - r) rs = rows - r;
        for (int dr = 0; dr < rs; dr++) {
            for (int dc = 0; dc < cs; dc++) {
                occupied[(r + dr) * cols + (c + dc)] = true;
            }
        }
        if (is_grid_spacer(child)) continue;  // occupies slots, renders nothing
        int x0 = area.x + boundary(area.w, c, cols), x1 = area.x + boundary(area.w, c + cs, cols);
        int y0 = area.y + boundary(area.h, r, rows), y1 = area.y + boundary(area.h, r + rs, rows);
        SolveStatus s = solve_node(child, Rect{x0, y0, x1 - x0, y1 - y0}, depth + 1, out);
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
