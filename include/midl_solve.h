#pragma once

// Pure-C++ integer layout solver for resolved single-class MIDL documents.
// Walks a layout node tree (leaf | split{dir|flow,children,weights} |
// grid{rows,cols,cells} | preset) and assigns an integer pixel rect to each
// leaf. Deterministic remainder distribution so this matches the web/Canvas
// renderer pixel-for-pixel (spec §3.3 "Layout solving contract").
//
// Host-testable: depends only on ArduinoJson. The device feeds it a
// PSRAM-allocated JsonDocument; recursion is bounded by midl::MAX_LAYOUT_DEPTH
// so solver frames are stack-safe.

#include <ArduinoJson.h>
#include <stddef.h>

#include "midl_limits.h"  // midl::MAX_LAYOUT_DEPTH, midl::FirmwareLimits

namespace midl {

struct Rect {
    int x, y, w, h;
};

struct Placement {
    char element[FirmwareLimits::str_len];  // element id (key in screen.elements)
    Rect rect;
};

// Fixed-capacity output (one screen). No dynamic allocation.
struct PlacementSet {
    Placement items[FirmwareLimits::max_tiles_per_screen];
    size_t count = 0;
};

enum SolveStatus {
    SOLVE_OK = 0,
    SOLVE_TOO_DEEP,        // nesting exceeded MAX_LAYOUT_DEPTH
    SOLVE_TOO_MANY_TILES,  // more leaves than max_tiles_per_screen
    SOLVE_BAD_NODE,        // a node matched none of leaf/split/grid/preset
    SOLVE_UNKNOWN_PRESET,  // preset name not in the built-in set
};

// Expand a preset node `{preset, slots}` into concrete geometry by solving it
// directly (the firmware never materialises an intermediate tree). Known
// presets: "full" (1 slot) and "hero-split" (3 slots, {1,{2,3}}).
// Returns SOLVE_BAD_NODE if slots missing/wrong count, SOLVE_UNKNOWN_PRESET if
// name unrecognised, SOLVE_TOO_MANY_TILES if out is full.
SolveStatus expand_preset(JsonVariantConst node, Rect area, PlacementSet &out);

// Solve one screen's `layout` node into leaf placements within `area`.
// `area` is the content rect for the class (full-frame in Slice 1; safe-area
// + gutter/padding arrive in Slice 2). Returns SOLVE_OK and fills `out`, or a
// failure code and leaves `out` partially filled (caller discards on failure).
SolveStatus solve_screen(JsonVariantConst layout, Rect area, PlacementSet &out);

}  // namespace midl
