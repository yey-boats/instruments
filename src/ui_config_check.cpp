#include "ui_config_check.h"

#include <string.h>

namespace ui_config {

bool is_valid_brightness(int value) {
    return value >= 0 && value <= 255;
}

bool is_valid_theme(const char *name) {
    if (!name) return false;
    // All six palettes are in the MIDL catalog (ui_theme.h). "auto" is a
    // legacy token the manager API accepts (persisted verbatim; renders as
    // night).
    return strcmp(name, "day") == 0 || strcmp(name, "night") == 0 ||
           strcmp(name, "high-contrast") == 0 || strcmp(name, "red-night") == 0 ||
           strcmp(name, "classic") == 0 || strcmp(name, "sun") == 0 || strcmp(name, "auto") == 0;
}

const char *theme_set_status(const char *theme) {
    // Same rule as ui.theme in apply_config, so the manager can select every
    // skin (sun / red-night / high-contrast / classic), not just day/night.
    return is_valid_theme(theme) ? "ok" : "invalid_payload";
}

}  // namespace ui_config
