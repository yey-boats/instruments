#include "ui_config_check.h"

#include <string.h>

namespace ui_config {

bool is_valid_brightness(int value) {
    return value >= 0 && value <= 255;
}

bool is_valid_theme(const char *name) {
    if (!name) return false;
    // day/night/high-contrast mirror the MIDL catalog; red-night/classic are
    // firmware-extra palettes (ui_theme.h). "auto" is a legacy token the
    // manager API accepts (persisted verbatim; renders as night).
    return strcmp(name, "day") == 0 || strcmp(name, "night") == 0 ||
           strcmp(name, "high-contrast") == 0 || strcmp(name, "red-night") == 0 ||
           strcmp(name, "classic") == 0 || strcmp(name, "auto") == 0;
}

}  // namespace ui_config
