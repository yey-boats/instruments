#include "ui_data.h"

#include "board_pins.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>

namespace ui {

static PosFormat s_fmt = PosFormat::DDM;
static constexpr uint8_t DEFAULT_BRIGHTNESS = 200;
static constexpr double DEFAULT_DEPTH_ALARM_M = 3.0;
static constexpr double DEFAULT_BATTERY_ALARM_V = 11.5;

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

static uint8_t clamp_brightness(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static bool s_brightness_loaded = false;
static uint8_t s_brightness = DEFAULT_BRIGHTNESS;

uint8_t brightness() {
    if (!s_brightness_loaded) {
        Preferences p;
        p.begin("ui", true);
        s_brightness = p.getUChar("bright", DEFAULT_BRIGHTNESS);
        p.end();
        s_brightness_loaded = true;
    }
    return s_brightness;
}

void set_brightness(int value) {
    uint8_t clamped = clamp_brightness(value);
    ledcWrite(0, clamped);
    s_brightness = clamped;
    s_brightness_loaded = true;
    Preferences p;
    p.begin("ui", false);
    p.putUChar("bright", clamped);
    p.end();
}

static double read_double_pref(const char *key, double fallback) {
    Preferences p;
    p.begin("ui", true);
    double value = p.getDouble(key, fallback);
    p.end();
    return isfinite(value) ? value : fallback;
}

static void write_double_pref(const char *key, double value) {
    Preferences p;
    p.begin("ui", false);
    p.putDouble(key, value);
    p.end();
}

// Alarm thresholds are read from the UI refresh loop (alarm_check at
// 5 Hz) and from screen_settings refresh. Reading NVS every call
// generated hundreds of log lines per second for missing-key errors
// (saturating Serial and dropping touch events). Cache in RAM, NVS is
// loaded once on first access and written through on changes.
static bool s_thresholds_loaded = false;
static double s_depth_alarm_m = DEFAULT_DEPTH_ALARM_M;
static double s_battery_alarm_v = DEFAULT_BATTERY_ALARM_V;

static void load_thresholds_once() {
    if (s_thresholds_loaded) return;
    s_depth_alarm_m = read_double_pref("depth_alarm_m", DEFAULT_DEPTH_ALARM_M);
    s_battery_alarm_v = read_double_pref("batt_alarm_v", DEFAULT_BATTERY_ALARM_V);
    s_thresholds_loaded = true;
}

double depth_alarm_m() {
    load_thresholds_once();
    return s_depth_alarm_m;
}

void set_depth_alarm_m(double value) {
    if (!isfinite(value)) value = DEFAULT_DEPTH_ALARM_M;
    if (value < 0.5) value = 0.5;
    if (value > 20.0) value = 20.0;
    s_depth_alarm_m = value;
    s_thresholds_loaded = true;
    write_double_pref("depth_alarm_m", value);
}

double battery_alarm_v() {
    load_thresholds_once();
    return s_battery_alarm_v;
}

void set_battery_alarm_v(double value) {
    if (!isfinite(value)) value = DEFAULT_BATTERY_ALARM_V;
    if (value < 9.0) value = 9.0;
    if (value > 14.0) value = 14.0;
    s_battery_alarm_v = value;
    s_thresholds_loaded = true;
    write_double_pref("batt_alarm_v", value);
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
