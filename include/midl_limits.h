#pragma once

// Single source of truth for the firmware-facing MIDL projection bounds.
// These mirror the layout POD limits (include/layout.h) and the values the
// square-480 capability manifest advertises. Plan 6's parser/solver and the
// host guard tests both read them from here so a POD change and a manifest
// change cannot silently diverge.

#include <stddef.h>

#include "layout.h"  // layout::MAX_SCREENS, MAX_TILES_PER_SCREEN, STR_LEN, PATH_LEN

namespace midl {

// Maximum nesting depth of a layout node tree the firmware solver will walk.
// Matches the square-480 manifest `maxDepth`. Enforced AS the solver descends
// (a post-hoc depth check would already have overflowed the 8 KB task stack —
// CLAUDE.md silent-reboot trap), so it is a hard recursion bound, not advice.
constexpr int MAX_LAYOUT_DEPTH = 3;

// A resolved single-class projection the device accepts must fit all of these.
struct FirmwareLimits {
    static constexpr size_t max_screens = layout::MAX_SCREENS;                    // 8
    static constexpr size_t max_tiles_per_screen = layout::MAX_TILES_PER_SCREEN;  // 4
    static constexpr int max_depth = MAX_LAYOUT_DEPTH;                            // 3
    static constexpr size_t str_len = layout::STR_LEN;                            // 32 (incl NUL)
    static constexpr size_t path_len = layout::PATH_LEN;                          // 96 (incl NUL)
};

}  // namespace midl
