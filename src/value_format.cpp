#include "value_format.h"

#include <math.h>
#include <stdio.h>

namespace vfmt {

const char *format_scaled(double v, const UnitFormat &f, char *buf, size_t cap) {
    if (!buf || cap == 0) return buf;
    if (!isfinite(v)) {
        snprintf(buf, cap, "--");
        return buf;
    }
    double scaled = v;
    const char *suffix = "";
    if (f.si_prefix) {
        double a = fabs(v);
        if (a >= 1e9) {
            scaled = v / 1e9;
            suffix = "G";
        } else if (a >= 1e6) {
            scaled = v / 1e6;
            suffix = "M";
        } else if (a >= 1e3) {
            scaled = v / 1e3;
            suffix = "k";
        }
    }
    snprintf(buf, cap, "%.*f%s", (int)f.decimals, scaled, suffix);
    return buf;
}

}  // namespace vfmt
