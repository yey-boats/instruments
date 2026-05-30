#pragma once

// Manager config parser per docs/specs/19 D2.
//
// Takes the JSON payload returned by /devices/:id/config and produces
// a validated, clamped RenderPlan POD that the UI layer can consume
// without re-traversing the original JSON.
//
// Pure C++ - host-testable. Depends only on ArduinoJson which is in
// the native env's lib_deps. The LVGL font resolver (D3) and widget
// registry (D4) consume RenderPlan downstream.

#include <ArduinoJson.h>
#include <stdint.h>

namespace manager_config {

// Limits per spec 19 §"Limits for v1".
static constexpr uint8_t MAX_WIDGETS = 32;
static constexpr uint8_t MAX_SCREENS = 16;
static constexpr uint8_t MAX_TILES_PER_SCREEN = 16;
static constexpr uint8_t MAX_WIDGET_ID = 31;
static constexpr uint8_t MAX_PATH = 95;
static constexpr uint8_t MAX_TITLE = 23;
static constexpr uint8_t MAX_UNIT = 11;

enum class WidgetType : uint8_t {
    Unknown = 0,
    Numeric,
    Text,
    Gauge,
    Compass,
    WindRose,
    Trend,
    Bar,
    Button,
    Autopilot,
};

WidgetType widget_type_from_string(const char *s);
const char *widget_type_to_string(WidgetType t);

struct WidgetStyle {
    uint16_t font_size = 0;  // 0 = inherit defaults
    uint16_t label_font_size = 0;
    uint16_t value_font_size = 0;
    uint16_t unit_font_size = 0;
};

struct WidgetDef {
    char id[MAX_WIDGET_ID + 1] = {0};
    WidgetType type = WidgetType::Unknown;
    char title[MAX_TITLE + 1] = {0};
    char path[MAX_PATH + 1] = {0};
    char unit[MAX_UNIT + 1] = {0};
    uint8_t precision = 0;
    WidgetStyle style;
    // gauge/bar bounds (NaN if unused)
    double min = 0.0 / 0.0;  // NaN
    double max = 0.0 / 0.0;
};

struct LayoutTile {
    char widget_id[MAX_WIDGET_ID + 1] = {0};
    uint8_t col = 0;
    uint8_t row = 0;
    uint8_t col_span = 1;
    uint8_t row_span = 1;
};

struct ScreenPlan {
    char id[MAX_WIDGET_ID + 1] = {0};
    uint8_t tile_count = 0;
    LayoutTile tiles[MAX_TILES_PER_SCREEN];
};

struct RenderPlan {
    char config_hash[72] = {0};
    char layout_variant[32] = {0};
    char widget_variant[32] = {0};
    uint16_t display_width = 0;
    uint16_t display_height = 0;
    uint8_t widget_count = 0;
    WidgetDef widgets[MAX_WIDGETS];
    uint8_t screen_count = 0;
    ScreenPlan screens[MAX_SCREENS];
    WidgetStyle defaults;
};

enum class ParseCode : uint8_t {
    Ok = 0,
    InvalidProtocol,
    WrongDevice,
    DisplayMismatch,
    UnsupportedLayoutType,
    UnsupportedWidgetType,
    MissingWidget,
    InvalidFontSize,
    TooManyWidgets,
    TooManyScreens,
    TooManyTiles,
    InvalidPath,
    InvalidAction,
    OutOfMemory,
    BadJson,
};

struct ParseError {
    ParseCode code = ParseCode::Ok;
    char path[64] = {0};
    char message[96] = {0};
};

const char *parse_code_to_string(ParseCode c);

// Parse and validate the config payload against the device's
// hardware capabilities. `cfg` is the `config` sub-document from
// /devices/:id/config; `device_width`/`device_height` are this
// device's actual display geometry (used for variant matching per
// spec 19 §Variant Matching).
//
// Returns Ok on success and fills `out`; otherwise sets `err` with
// code + path + message and leaves `out` untouched.
//
// Unknown widget references are dropped silently (per spec); only
// screens that reference unsupported widget TYPES are rejected.
bool parse(JsonObjectConst cfg, uint16_t device_width, uint16_t device_height, RenderPlan &out,
           ParseError &err);

}  // namespace manager_config
