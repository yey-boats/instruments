#pragma once

// Test-only override hooks for the BOARD_ID_NATIVE_FAKE board impl.
//
// Compiled only into the native test env. Including this header from
// production code is intentionally awkward (it lives under boards/
// rather than at include/ root) because nothing in production should
// be reaching into a test fake.

#include <stdint.h>

namespace board {
namespace native_fake {

// Override the geometry returned by board::geometry(). Subsequent
// ui::layout_context() calls observe the new values. Pass 0 to any
// field to keep the current value.
void set_geometry(uint16_t width_px, uint16_t height_px, uint16_t diagonal_tenths_in);

// Restore the compile-time defaults (FAKE_BOARD_WIDTH / HEIGHT / DIAG).
void reset_geometry();

}  // namespace native_fake
}  // namespace board
