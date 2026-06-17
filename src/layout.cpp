#include "layout.h"

#include <string.h>

namespace layout {

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) {
        if (cap) dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

TileType parse_tile_type(const char *s) {
    if (!s) return TILE_UNKNOWN;
    if (strcmp(s, "wind") == 0) return TILE_WIND;
    if (strcmp(s, "nav") == 0) return TILE_NAV;
    if (strcmp(s, "depth_temp") == 0) return TILE_DEPTH_TEMP;
    if (strcmp(s, "device_status") == 0) return TILE_DEVICE_STATUS;
    if (strcmp(s, "big_number") == 0) return TILE_BIG_NUMBER;
    if (strcmp(s, "compass") == 0) return TILE_COMPASS;
    return TILE_UNKNOWN;
}

ScreenType parse_screen_type(const char *s) {
    if (!s) return SCREEN_UNKNOWN;
    if (strcmp(s, "quadrants") == 0) return SCREEN_QUADRANTS;
    if (strcmp(s, "steering") == 0) return SCREEN_STEERING;
    if (strcmp(s, "autopilot") == 0) return SCREEN_AUTOPILOT;
    if (strcmp(s, "route") == 0) return SCREEN_ROUTE;
    if (strcmp(s, "trip") == 0) return SCREEN_TRIP;
    if (strcmp(s, "chart") == 0) return SCREEN_CHART;
    return SCREEN_UNKNOWN;
}

AlarmLevel parse_alarm_level(const char *s) {
    if (!s) return ALARM_LVL_WARN;
    if (strcmp(s, "info") == 0) return ALARM_LVL_INFO;
    if (strcmp(s, "warn") == 0) return ALARM_LVL_WARN;
    if (strcmp(s, "alarm") == 0) return ALARM_LVL_ALARM;
    if (strcmp(s, "emergency") == 0) return ALARM_LVL_EMERGENCY;
    return ALARM_LVL_WARN;
}

static void parse_paths(JsonVariantConst v, PathBinding *out, size_t &count, size_t cap) {
    count = 0;
    if (v.isNull()) return;
    if (v.is<JsonObjectConst>()) {
        JsonObjectConst obj = v.as<JsonObjectConst>();
        for (JsonPairConst kv : obj) {
            if (count >= cap) break;
            copy_str(out[count].key, STR_LEN, kv.key().c_str());
            const char *p = kv.value().as<const char *>();
            copy_str(out[count].path, PATH_LEN, p);
            ++count;
        }
    }
}

static void parse_tile(JsonObjectConst t, Tile &out) {
    copy_str(out.id, STR_LEN, t["id"].as<const char *>());
    copy_str(out.title, STR_LEN, t["title"].as<const char *>());
    out.type = parse_tile_type(t["type"].as<const char *>());
    parse_paths(t["paths"], out.paths, out.path_count, MAX_PATHS_PER_OBJECT);
    // Editor-style fields: `widget` is the per-tile widget id string,
    // `primary` and `secondary` are the bound SK paths.
    copy_str(out.widget, STR_LEN, t["widget"].as<const char *>());
    copy_str(out.primary_path, PATH_LEN, t["primary"].as<const char *>());
    copy_str(out.secondary_path, PATH_LEN, t["secondary"].as<const char *>());
    // Zoom: `zoomable` defaults per widget kind (numeric → true) when absent;
    // `zoom` defaults to "auto" for a zoomable field with no explicit target.
    JsonVariantConst zable = t["zoomable"];
    out.zoomable = zable.isNull() ? default_zoomable(out.widget) : zable.as<bool>();
    const char *zoom = t["zoom"].as<const char *>();
    if (zoom && zoom[0])
        copy_str(out.zoom, STR_LEN, zoom);
    else if (out.zoomable)
        copy_str(out.zoom, STR_LEN, "auto");
}

static void parse_screen(JsonObjectConst s, Screen &out) {
    copy_str(out.id, STR_LEN, s["id"].as<const char *>());
    copy_str(out.title, STR_LEN, s["title"].as<const char *>());
    out.type = parse_screen_type(s["type"].as<const char *>());
    parse_paths(s["paths"], out.paths, out.path_count, MAX_PATHS_PER_OBJECT);
    JsonArrayConst tiles = s["tiles"].as<JsonArrayConst>();
    out.tile_count = 0;
    if (!tiles.isNull()) {
        for (JsonObjectConst t : tiles) {
            if (out.tile_count >= MAX_TILES_PER_SCREEN) break;
            parse_tile(t, out.tiles[out.tile_count++]);
        }
    }
}

static void parse_alarm(JsonObjectConst a, AlarmRule &out) {
    copy_str(out.id, STR_LEN, a["id"].as<const char *>());
    copy_str(out.path, PATH_LEN, a["path"].as<const char *>());
    out.level = parse_alarm_level(a["level"].as<const char *>());
    JsonVariantConst lt = a["lt"];
    if (!lt.isNull()) {
        out.has_lt = true;
        out.lt = lt.as<double>();
    }
    JsonVariantConst gt = a["gt"];
    if (!gt.isNull()) {
        out.has_gt = true;
        out.gt = gt.as<double>();
    }
    copy_str(out.message, MSG_LEN, a["message"].as<const char *>());
}

int parse(const char *json, size_t len, Config &out) {
    // Reset in place - assigning Config{} would create a temporary ~34 KB
    // object on the stack and overflow Arduino's 8 KB main task stack.
    memset(&out, 0, sizeof(out));
    out.settings.demo_period_ms = 3000;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) return -1;

    out.version = doc["version"].as<int>();

    JsonObjectConst settings = doc["settings"].as<JsonObjectConst>();
    if (!settings.isNull()) {
        copy_str(out.settings.default_screen, STR_LEN,
                 settings["default_screen"].as<const char *>());
        JsonVariantConst p = settings["demo_period_ms"];
        if (!p.isNull()) out.settings.demo_period_ms = p.as<uint32_t>();
    }

    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    if (!screens.isNull()) {
        for (JsonObjectConst s : screens) {
            if (out.screen_count >= MAX_SCREENS) break;
            parse_screen(s, out.screens[out.screen_count++]);
        }
    }

    JsonArrayConst alarms = doc["alarms"].as<JsonArrayConst>();
    if (!alarms.isNull()) {
        for (JsonObjectConst a : alarms) {
            if (out.alarm_count >= MAX_ALARMS) break;
            parse_alarm(a, out.alarms[out.alarm_count++]);
        }
    }

    return 0;
}

bool default_zoomable(const char *widget) {
    // Numeric fields default zoomable; an empty/unknown widget is rendered as
    // numeric (see ui::layout_render::widget_to_kind) so it inherits the same
    // default. Every explicitly non-numeric widget defaults to not zoomable.
    if (!widget || !widget[0]) return true;
    return strcmp(widget, "numeric") == 0;
}

ZoomAction zoom_action(bool zoomable, const char *zoom) {
    if (!zoomable) return ZOOM_NONE;
    if (!zoom || !zoom[0] || strcmp(zoom, "auto") == 0) return ZOOM_AUTO_SCALE;
    return ZOOM_SHOW_SCREEN;
}

const Screen *find_screen(const Config &cfg, const char *id) {
    if (!id) return nullptr;
    for (size_t i = 0; i < cfg.screen_count; ++i) {
        if (strcmp(cfg.screens[i].id, id) == 0) return &cfg.screens[i];
    }
    return nullptr;
}

}  // namespace layout
