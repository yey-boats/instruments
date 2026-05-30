#include "hostname_check.h"

#include <string.h>

namespace hostname_check {

bool is_valid(const char *s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len == 0 || len > MAX_LEN) return false;
    // Edge dashes are rejected to avoid `-foo` / `foo-` that some
    // mDNS implementations refuse to resolve.
    if (s[0] == '-' || s[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace hostname_check
