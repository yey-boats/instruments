#pragma once

// Settings THEME row mapping, shared by the built-in Settings screen
// (src/ui/screen_settings.cpp) and the MIDL setup_form template
// (src/ui/ui_layouts.cpp). Pure and header-only so it is host-tested
// (test/test_theme_choice) and links into every env without filter edits.
//
// DAY / NIGHT pick the sunlight and night-vision skins (owner choice,
// 2026-11-14, instruments#61); AUTO stays the legacy manager token (persisted
// verbatim, renders as night). Legacy persisted "day" / "night" highlight the
// DAY / NIGHT button so a device upgraded from before #61 still shows its row.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <string.h>

namespace theme_choice {

enum : int { kDay = 0, kNight = 1, kAuto = 2, kCount = 3, kNone = -1 };

// Theme name each button selects, in button order (DAY, NIGHT, AUTO).
inline const char *const *themes() {
    static const char *const kThemes[kCount] = {"sun", "red-night", "auto"};
    return kThemes;
}

// Theme name for button `index`, or nullptr when out of range.
inline const char *theme_for(int index) {
    return (index >= 0 && index < kCount) ? themes()[index] : nullptr;
}

// Button to highlight for a persisted theme name: kDay for "sun" / legacy
// "day", kNight for "red-night" / legacy "night", kAuto for "auto", kNone for
// anything else (high-contrast, classic, unknown, null) — no button matches.
inline int index_for(const char *theme) {
    if (!theme) return kNone;
    if (strcmp(theme, "sun") == 0 || strcmp(theme, "day") == 0) return kDay;
    if (strcmp(theme, "red-night") == 0 || strcmp(theme, "night") == 0) return kNight;
    if (strcmp(theme, "auto") == 0) return kAuto;
    return kNone;
}

}  // namespace theme_choice
