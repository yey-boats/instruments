#include "ui_data.h"

#include <Preferences.h>
#include <stdio.h>

namespace ui {

static PosFormat s_fmt = PosFormat::DDM;

const char *pos_format_name(PosFormat f) {
    switch (f) {
    case PosFormat::DDM: return "ddm";
    case PosFormat::DD:  return "dd";
    case PosFormat::DMS: return "dms";
    }
    return "?";
}

PosFormat pos_format() { return s_fmt; }

void set_pos_format(PosFormat f) {
    s_fmt = f;
    Preferences p;
    p.begin("ui", false);
    p.putUChar("pos_fmt", (uint8_t)f);
    p.end();
}

void format_position(double lat, double lon, PosFormat fmt, char *buf, size_t cap) {
    char ns = lat >= 0 ? 'N' : 'S';
    char ew = lon >= 0 ? 'E' : 'W';
    double la = fabs(lat);
    double lo = fabs(lon);
    switch (fmt) {
    case PosFormat::DDM: {
        int la_d = (int)la;
        double la_m = (la - la_d) * 60.0;
        int lo_d = (int)lo;
        double lo_m = (lo - lo_d) * 60.0;
        snprintf(buf, cap, "%02d\xC2\xB0%06.3f'%c\n%03d\xC2\xB0%06.3f'%c", la_d, la_m, ns, lo_d,
                 lo_m, ew);
        break;
    }
    case PosFormat::DD:
        snprintf(buf, cap, "%.4f\xC2\xB0%c\n%.4f\xC2\xB0%c", la, ns, lo, ew);
        break;
    case PosFormat::DMS: {
        int la_d = (int)la;
        double la_r = (la - la_d) * 60.0;
        int la_m = (int)la_r;
        double la_s = (la_r - la_m) * 60.0;
        int lo_d = (int)lo;
        double lo_r = (lo - lo_d) * 60.0;
        int lo_m = (int)lo_r;
        double lo_s = (lo_r - lo_m) * 60.0;
        snprintf(buf, cap, "%d\xC2\xB0%02d'%04.1f\"%c\n%d\xC2\xB0%02d'%04.1f\"%c", la_d, la_m,
                 la_s, ns, lo_d, lo_m, lo_s, ew);
        break;
    }
    }
}

}  // namespace ui
