#include "layout_loader.h"
#include "net.h"

namespace layout {

// Baked-in default layout. Mirrors what the firmware currently renders
// so the parsed Config is immediately useful as a runtime model even
// before the SignalK fetcher lands. Keep this in sync with the on-screen
// reality until the renderer takes over.
static const char *DEFAULT_LAYOUT_JSON =
    "{"
    "\"version\":1,"
    "\"settings\":{\"default_screen\":\"dashboard\",\"demo_period_ms\":3000},"
    "\"screens\":[{"
    "\"id\":\"dashboard\",\"title\":\"Dashboard\",\"type\":\"quadrants\","
    "\"tiles\":["
    "{\"id\":\"wind\",\"title\":\"WIND\",\"type\":\"wind\","
    "\"paths\":{"
    "\"awa\":\"environment.wind.angleApparent\","
    "\"aws\":\"environment.wind.speedApparent\""
    "}},"
    "{\"id\":\"nav\",\"title\":\"NAV\",\"type\":\"nav\","
    "\"paths\":{"
    "\"sog\":\"navigation.speedOverGround\","
    "\"cog\":\"navigation.courseOverGroundTrue\","
    "\"hdg\":\"navigation.headingTrue\","
    "\"position\":\"navigation.position\""
    "}},"
    "{\"id\":\"depth\",\"title\":\"DEPTH / TEMP\",\"type\":\"depth_temp\","
    "\"paths\":{"
    "\"depth\":\"environment.depth.belowTransducer\","
    "\"temp\":\"environment.water.temperature\""
    "}},"
    "{\"id\":\"status\",\"title\":\"STATUS\",\"type\":\"device_status\"}"
    "]"
    "}],"
    "\"alarms\":["
    "{\"id\":\"shallow\",\"path\":\"environment.depth.belowTransducer\","
    "\"level\":\"alarm\",\"lt\":3.0,\"message\":\"SHALLOW WATER\"},"
    "{\"id\":\"batt_low\",\"path\":\"electrical.batteries.house.voltage\","
    "\"level\":\"warn\",\"lt\":11.5,\"message\":\"BATTERY LOW\"}"
    "]"
    "}";

static Config s_current;
static bool s_loaded = false;

bool load_default() {
    int rc = parse(DEFAULT_LAYOUT_JSON, strlen(DEFAULT_LAYOUT_JSON), s_current);
    if (rc != 0) {
        net::logf("[layout] default parse FAILED (rc=%d)", rc);
        return false;
    }
    s_loaded = true;
    net::logf("[layout] default loaded: %u screens, %u alarms", (unsigned)s_current.screen_count,
              (unsigned)s_current.alarm_count);
    return true;
}

const Config &current() {
    return s_current;
}

static const char *screen_type_name(ScreenType t) {
    switch (t) {
    case SCREEN_QUADRANTS:
        return "quadrants";
    case SCREEN_STEERING:
        return "steering";
    case SCREEN_AUTOPILOT:
        return "autopilot";
    case SCREEN_ROUTE:
        return "route";
    case SCREEN_TRIP:
        return "trip";
    case SCREEN_CHART:
        return "chart";
    default:
        return "?";
    }
}

static const char *tile_type_name(TileType t) {
    switch (t) {
    case TILE_WIND:
        return "wind";
    case TILE_NAV:
        return "nav";
    case TILE_DEPTH_TEMP:
        return "depth_temp";
    case TILE_DEVICE_STATUS:
        return "device_status";
    case TILE_BIG_NUMBER:
        return "big_number";
    case TILE_COMPASS:
        return "compass";
    default:
        return "?";
    }
}

void show_summary() {
    if (!s_loaded) {
        net::logf("[layout] not loaded");
        return;
    }
    net::logf("[layout] version=%d default=%s demo=%lums", s_current.version,
              s_current.settings.default_screen, (unsigned long)s_current.settings.demo_period_ms);
    for (size_t i = 0; i < s_current.screen_count; ++i) {
        const Screen &s = s_current.screens[i];
        net::logf("[layout] screen %u id=%s type=%s tiles=%u", (unsigned)i, s.id,
                  screen_type_name(s.type), (unsigned)s.tile_count);
        for (size_t j = 0; j < s.tile_count; ++j) {
            const Tile &t = s.tiles[j];
            net::logf("[layout]   tile %u id=%s type=%s paths=%u", (unsigned)j, t.id,
                      tile_type_name(t.type), (unsigned)t.path_count);
        }
    }
    for (size_t i = 0; i < s_current.alarm_count; ++i) {
        const AlarmRule &a = s_current.alarms[i];
        net::logf("[layout] alarm %s path=%s lt=%g msg=%s", a.id, a.path, a.has_lt ? a.lt : 0.0,
                  a.message);
    }
}

bool handleSerialCommand(const String &line) {
    if (line == "layout-show" || line == "layout") {
        show_summary();
        return true;
    }
    if (line == "layout-reload-default") {
        load_default();
        return true;
    }
    return false;
}

}  // namespace layout
