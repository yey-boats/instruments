// Pure painter decisions (no LVGL) used by src/ui/ui_layouts.cpp, so the rules
// the painters apply are host-tested (test/test_paint_rules) rather than
// re-implemented in the tests (instruments#112).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdint.h>
#include <string.h>

namespace ui {
namespace paint {

// ---- wind dial bezel separators (build_bezel) ------------------------------
// Eight separators between the eight cardinal/intercardinal labels. Every
// separator sits between one cardinal and one intercardinal label (a tick ON a
// label would cut through its glyphs: ticks span r_bezel-4 inward, labels sit
// at r_bezel-13), so they are all alike: cardinals are told apart by their
// larger, stronger label (build_cardinals), not by an uneven tick (#112: #104
// keyed "cardinal" on the label angle but drew at +22, so each N/E/S/W got one
// heavy and one light flank).
struct BezelTick {
    int angle_deg;  // screen angle of the tick
    int len;
    int width;
    bool strong;  // band_ink (true) or band_ink_dim
};
inline BezelTick bezel_tick(int k) {
    return {k * 45 + 22, 10, 2, false};
}

// ---- ink on a coloured fill (paint_button_body, badges) --------------------
// Dark or light ink by the fill's luminance (BT.601 luma), as badge_ink().
inline uint32_t ink_for_fill(uint32_t fill, uint32_t dark, uint32_t light) {
    uint32_t r = (fill >> 16) & 0xFF, g = (fill >> 8) & 0xFF, b = fill & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 >= 96 ? dark : light;
}

// ---- compass captions (paint_compass_body) ----------------------------------
// The small caption above the heading number, and the first legend row: a short
// quantity tag (#111: the authored name, so a TWD compass reads "TWD"), never the
// tile caption again. "HDG" when the element is unnamed (its label is then its
// raw id) or the name doesn't fit max_chars; a composite name ("HDG / CTS",
// "COG/SOG") gives its first part.
inline const char *compass_caption(const char *label, const char *id, char *buf, size_t cap,
                                   size_t max_chars) {
    if (!cap) return "HDG";
    if (!label || !label[0] || (id && strcmp(label, id) == 0)) return "HDG";
    size_t n = strcspn(label, "/");
    while (n > 0 && label[n - 1] == ' ')
        --n;  // "HDG / CTS" -> "HDG"
    if (n == 0 || n > max_chars || n >= cap) return "HDG";
    memcpy(buf, label, n);
    buf[n] = 0;
    return buf;
}

// ---- round-fit value-only fallback (paint_value_only_body) -----------------
// Whether the unit rides beside the value as a separate word. Degrees never do
// (#112: #111's default "deg" on every compass made the fallback draw "046 deg"
// and shrink the number): a heading reads as a number.
inline bool value_only_word_unit(bool is_gauge, bool is_compass, const char *unit, bool side,
                                 double range_min, double range_max) {
    if (!unit || !unit[0]) return false;
    if (is_compass || strcmp(unit, "deg") == 0) return false;
    bool glued = unit[0] == '%' || unit[0] == '\xC2';
    return !(is_gauge && (glued || side || range_min == range_max));
}

}  // namespace paint
}  // namespace ui
