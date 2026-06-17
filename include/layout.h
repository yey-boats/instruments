#pragma once

// Layout configuration: parsed representation of the multi-screen JSON config
// that lives on the SignalK server (path `configuration.boat-mfd.layouts`).
// Pure C++; compiles on host for unit tests as well as the device.

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace layout {

// Fixed bounds keep the whole Config in-place (no heap allocations).
constexpr size_t MAX_SCREENS = 8;
constexpr size_t MAX_TILES_PER_SCREEN = 4;
constexpr size_t MAX_PATHS_PER_OBJECT = 6;
constexpr size_t MAX_ALARMS = 8;
constexpr size_t STR_LEN = 32;
constexpr size_t PATH_LEN = 96;
constexpr size_t MSG_LEN = 48;

enum TileType {
    TILE_UNKNOWN = 0,
    TILE_WIND,           // AWA dial + AWS
    TILE_NAV,            // SOG / COG / HDG / pos
    TILE_DEPTH_TEMP,     // depth + water temp
    TILE_DEVICE_STATUS,  // battery / IP / RSSI / sk state
    TILE_BIG_NUMBER,     // single SignalK path, big digits
    TILE_COMPASS,        // analog compass card
};

enum ScreenType {
    SCREEN_UNKNOWN = 0,
    SCREEN_QUADRANTS,  // 2x2 grid of tiles
    SCREEN_STEERING,   // heading bug + CTS + XTE
    SCREEN_AUTOPILOT,  // AP control + state
    SCREEN_ROUTE,      // DTW/BTW/CTS/XTE/VMG
    SCREEN_TRIP,       // distance / time / averages
    SCREEN_CHART,      // raster chart from SignalK chart plugin
};

enum AlarmLevel {
    ALARM_LVL_INFO = 0,
    ALARM_LVL_WARN,
    ALARM_LVL_ALARM,
    ALARM_LVL_EMERGENCY,
};

struct PathBinding {
    char key[STR_LEN] = {0};
    char path[PATH_LEN] = {0};
};

struct Tile {
    char id[STR_LEN] = {0};
    char title[STR_LEN] = {0};
    TileType type = TILE_UNKNOWN;
    PathBinding paths[MAX_PATHS_PER_OBJECT];
    size_t path_count = 0;
    // Editor-style tile fields. `widget` is the same string the layout
    // editor emits ("numeric", "compass", "gauge", "bar", "windRose",
    // "autopilot", "text", "button", "trend"). `primary_path` /
    // `secondary_path` carry SK path strings the editor binds to the
    // tile. Empty strings mean the field is unbound; the renderer
    // falls back to the legacy TileType + paths[] map.
    char widget[STR_LEN] = {0};
    char primary_path[PATH_LEN] = {0};
    char secondary_path[PATH_LEN] = {0};
    // Per-field zoom (spec §Zoom). `zoomable` gates the tap: numeric fields
    // default true, every other widget defaults false (resolved at parse
    // time). `zoom` is the target: "auto" scales the field in place on the
    // device zoom screen; a "<screenId>" string opens that full screen.
    bool zoomable = false;
    char zoom[STR_LEN] = {0};
};

struct Screen {
    char id[STR_LEN] = {0};
    char title[STR_LEN] = {0};
    ScreenType type = SCREEN_UNKNOWN;
    Tile tiles[MAX_TILES_PER_SCREEN];
    size_t tile_count = 0;
    PathBinding paths[MAX_PATHS_PER_OBJECT];
    size_t path_count = 0;
};

struct AlarmRule {
    char id[STR_LEN] = {0};
    char path[PATH_LEN] = {0};
    AlarmLevel level = ALARM_LVL_WARN;
    bool has_lt = false;
    double lt = 0;
    bool has_gt = false;
    double gt = 0;
    char message[MSG_LEN] = {0};
};

struct Settings {
    char default_screen[STR_LEN] = {0};
    uint32_t demo_period_ms = 3000;
};

struct Config {
    int version = 0;
    Settings settings;
    Screen screens[MAX_SCREENS];
    size_t screen_count = 0;
    AlarmRule alarms[MAX_ALARMS];
    size_t alarm_count = 0;
};

// Parse a layout JSON document into `out`. `out` is reset to defaults first.
// Returns 0 on success, negative on parse/structure error.
int parse(const char *json, size_t len, Config &out);

// Name -> enum helpers. Return *_UNKNOWN if the string doesn't match.
TileType parse_tile_type(const char *s);
ScreenType parse_screen_type(const char *s);
AlarmLevel parse_alarm_level(const char *s);

// Find a screen by id. Returns nullptr if not found.
const Screen *find_screen(const Config &cfg, const char *id);

// ---- Per-field zoom decision (pure; host-tested) -------------------------

// What a tap on a (possibly zoomable) field should do.
enum ZoomAction {
    ZOOM_NONE = 0,     // field is not zoomable; tap does nothing
    ZOOM_AUTO_SCALE,   // scale the field up in place on the zoom screen
    ZOOM_SHOW_SCREEN,  // switch to the referenced full screen
};

// Default `zoomable` for a widget kind when the config omits the key.
// Numeric (and empty/unknown, which the renderer treats as numeric) default
// to true; every other widget defaults to false. Mirrors the manifest's
// per-viewType zoom support in the spec.
bool default_zoomable(const char *widget);

// Resolve a tap action from a tile's resolved `zoomable` flag and `zoom`
// target string. Not zoomable → NONE. Zoomable with an empty or "auto"
// target → AUTO_SCALE. Zoomable with any other (non-empty) target → SHOW_SCREEN
// (the caller validates the screen id exists before navigating).
ZoomAction zoom_action(bool zoomable, const char *zoom);

}  // namespace layout
