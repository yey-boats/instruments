#include "config_model.h"

#include <string.h>
#include <math.h>

namespace config {

bool clamp_ui(UiConfig &c) {
    bool ok = true;
    if (c.brightness < 20) {
        c.brightness = 20;
        ok = false;
    }
    if (c.brightness > 255) {
        c.brightness = 255;
        ok = false;
    }
    if ((uint8_t)c.pos_format > (uint8_t)PosFormat::DMS) {
        c.pos_format = PosFormat::DDM;
        ok = false;
    }
    if ((uint8_t)c.theme > (uint8_t)Theme::Classic) {
        c.theme = Theme::Night;
        ok = false;
    }
    c.default_screen[sizeof(c.default_screen) - 1] = 0;
    return ok;
}

bool clamp_alarms(AlarmConfig &c) {
    bool ok = true;
    if (!isfinite(c.depth_alarm_m)) {
        c.depth_alarm_m = 3.0;
        ok = false;
    }
    if (c.depth_alarm_m < 0.5) {
        c.depth_alarm_m = 0.5;
        ok = false;
    }
    if (c.depth_alarm_m > 20.0) {
        c.depth_alarm_m = 20.0;
        ok = false;
    }
    if (!isfinite(c.battery_alarm_v)) {
        c.battery_alarm_v = 11.5;
        ok = false;
    }
    if (c.battery_alarm_v < 9.0) {
        c.battery_alarm_v = 9.0;
        ok = false;
    }
    if (c.battery_alarm_v > 14.0) {
        c.battery_alarm_v = 14.0;
        ok = false;
    }
    return ok;
}

bool clamp_signalk(SignalKConfig &c) {
    bool ok = true;
    c.host[sizeof(c.host) - 1] = 0;
    c.token[sizeof(c.token) - 1] = 0;
    if (c.port == 0) {
        c.port = 3000;
        ok = false;
    }
    return ok;
}

const char *theme_name(Theme t) {
    switch (t) {
    case Theme::Night:
        return "night";
    case Theme::Day:
        return "day";
    case Theme::HighContrast:
        return "high-contrast";
    case Theme::RedNight:
        return "red-night";
    case Theme::Classic:
        return "classic";
    }
    return "night";
}

Theme parse_theme(const char *s, Theme fallback) {
    if (!s) return fallback;
    if (strcmp(s, "day") == 0) return Theme::Day;
    if (strcmp(s, "night") == 0) return Theme::Night;
    if (strcmp(s, "high-contrast") == 0) return Theme::HighContrast;
    if (strcmp(s, "red-night") == 0) return Theme::RedNight;
    if (strcmp(s, "classic") == 0) return Theme::Classic;
    return fallback;
}

const char *pos_format_name(PosFormat f) {
    switch (f) {
    case PosFormat::DDM:
        return "ddm";
    case PosFormat::DD:
        return "dd";
    case PosFormat::DMS:
        return "dms";
    }
    return "ddm";
}

PosFormat parse_pos_format(const char *s, PosFormat fallback) {
    if (!s) return fallback;
    if (strcmp(s, "ddm") == 0) return PosFormat::DDM;
    if (strcmp(s, "dd") == 0) return PosFormat::DD;
    if (strcmp(s, "dms") == 0) return PosFormat::DMS;
    return fallback;
}

const char *domain_name(Domain d) {
    switch (d) {
    case Domain::Ui:
        return "ui";
    case Domain::Alarms:
        return "alarms";
    case Domain::SignalK:
        return "signalk";
    default:
        return "?";
    }
}

const char *source_name(Source s) {
    switch (s) {
    case Source::Default:
        return "default";
    case Source::Storage:
        return "storage";
    case Source::Web:
        return "web";
    case Source::Ble:
        return "ble";
    case Source::Serial:
        return "serial";
    case Source::External:
        return "external";
    }
    return "?";
}

}  // namespace config
