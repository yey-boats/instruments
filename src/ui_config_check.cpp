#include "ui_config_check.h"

#include <string.h>

namespace ui_config {

bool is_valid_brightness(int value) {
    return value >= 0 && value <= 255;
}

bool is_valid_theme(const char *name) {
    if (!name) return false;
    return strcmp(name, "day") == 0 || strcmp(name, "night") == 0 || strcmp(name, "auto") == 0;
}

}  // namespace ui_config
