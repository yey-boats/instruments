#pragma once
// SIM_THEME hook shared by every sim_*.cpp harness main.
//
// The headless render harnesses pick their palette from the SIM_THEME
// environment variable ("night" | "day" | "high-contrast" | "red-night" |
// "classic", see src/ui/ui_theme.cpp) so a single built binary can be swept
// across the whole theme matrix by tools/render_all_resolutions.sh without
// per-theme rebuilds or argv reshuffling in seven different mains.
//
// Call apply_theme_from_env() at the TOP of main(), before any screen is
// built — painters read the global ui::theme palette at build time.
// ui::use_theme() only assigns a struct, so calling it before lv_init() is
// safe. Unset/empty SIM_THEME keeps the default night palette.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstdio>
#include <cstdlib>

#include "ui_theme.h"

namespace sim {

// Returns false (palette untouched, message on stderr) for an unknown name so
// the caller can exit nonzero instead of silently rendering the default.
inline bool apply_theme_from_env() {
    const char *name = std::getenv("SIM_THEME");
    if (!name || !*name) return true;  // default night palette
    if (!ui::use_theme(name)) {
        std::fprintf(stderr, "unknown SIM_THEME '%s' (night|day|high-contrast|red-night|classic)\n",
                     name);
        return false;
    }
    std::printf("theme: %s\n", ui::theme_id());
    return true;
}

}  // namespace sim
