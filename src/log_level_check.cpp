#include "log_level_check.h"

#include <string.h>

namespace log_level_check {

bool from_string(const char *s, int *out) {
    if (!s || !*s || !out) return false;
    if (!strcmp(s, "none"))    { *out = 0; return true; }
    if (!strcmp(s, "error"))   { *out = 1; return true; }
    if (!strcmp(s, "warn"))    { *out = 2; return true; }
    if (!strcmp(s, "info"))    { *out = 3; return true; }
    if (!strcmp(s, "debug"))   { *out = 4; return true; }
    if (!strcmp(s, "trace"))   { *out = 5; return true; }
    if (!strcmp(s, "verbose")) { *out = 5; return true; }
    return false;
}

bool is_valid_int(int level) {
    return level >= kMin && level <= kMax;
}

}  // namespace log_level_check
