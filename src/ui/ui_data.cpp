#include "ui_data.h"

#include "board.h"
#include "board_pins.h"
#include "config_runtime.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>

namespace ui {

// The UI data getters/setters are now thin adapters over config_runtime
// per docs/specs/08. Live values are RAM-resident; writes go through a
// typed mutation and are persisted asynchronously by the storage
// worker (debounced to coalesce rapid Settings taps).

const char *pos_format_name(PosFormat f) {
    return ::config::pos_format_name((::config::PosFormat)(uint8_t)f);
}

PosFormat pos_format() {
    return (PosFormat)(uint8_t)::config::ui().pos_format;
}

void set_pos_format(PosFormat f) {
    ::config::Mutation m;
    m.kind = ::config::MutationKind::SetPosFormat;
    m.pos_format = (::config::PosFormat)(uint8_t)f;
    m.source = ::config::Source::Serial;
    ::config::mutate(m);
}

uint8_t brightness() {
    return ::config::ui().brightness;
}

void set_brightness(int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    // Apply live to the LED driver immediately (RAM-first principle:
    // the panel responds the moment the user moves the slider). The
    // mutation queues a debounced NVS write so repeated taps coalesce.
    ledcWrite(0, (uint8_t)value);
    ::config::Mutation m;
    m.kind = ::config::MutationKind::SetBrightness;
    m.u8 = (uint8_t)value;
    m.source = ::config::Source::Serial;
    ::config::mutate(m);
}

double depth_alarm_m() {
    return ::config::alarms().depth_alarm_m;
}

void set_depth_alarm_m(double value) {
    ::config::Mutation m;
    m.kind = ::config::MutationKind::SetDepthAlarm;
    m.d = value;
    m.source = ::config::Source::Serial;
    ::config::mutate(m);
}

double battery_alarm_v() {
    return ::config::alarms().battery_alarm_v;
}

void set_battery_alarm_v(double value) {
    ::config::Mutation m;
    m.kind = ::config::MutationKind::SetBatteryAlarm;
    m.d = value;
    m.source = ::config::Source::Serial;
    ::config::mutate(m);
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
