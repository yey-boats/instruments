#pragma once

// Pure numeric→display formatter. No Arduino deps — compiles for the firmware
// and the native host (unit-tested in test/test_value_format).
//
// A value is formatted with a fixed number of fractional digits and, when
// `si_prefix` is set, large magnitudes are scaled to an SI prefix so they fit
// the small tile labels: 1234.5 -> "1.23k", 2841 -> "2.8k", 1.5e6 -> "1.50M".
// The prefix letter is appended to the number; the unit label (nm/m/…) stays a
// separate, caller-owned string, so "1.23k" + " nm" reads "1.23k nm".

#include <stddef.h>
#include <stdint.h>

namespace vfmt {

// Per-unit-class display format. Held in config::FormatConfig and threaded into
// the painters. Defaults live in config_model.h.
struct UnitFormat {
    uint8_t decimals = 1;    // fractional digits after any scaling
    bool si_prefix = false;  // scale |v|>=1000 to k / M / G
    constexpr UnitFormat() = default;
    constexpr UnitFormat(uint8_t d, bool si) : decimals(d), si_prefix(si) {}
};

// Format `v` into `buf` (always NUL-terminated). NaN/inf → "--". Returns buf.
const char *format_scaled(double v, const UnitFormat &f, char *buf, size_t cap);

}  // namespace vfmt
