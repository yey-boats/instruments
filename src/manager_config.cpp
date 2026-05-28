#include "manager_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace manager_config {

namespace {

void set_err(ParseError &err, ParseCode code, const char *path,
             const char *message) {
    err.code = code;
    if (path) {
        strncpy(err.path, path, sizeof(err.path) - 1);
        err.path[sizeof(err.path) - 1] = 0;
    } else {
        err.path[0] = 0;
    }
    if (message) {
        strncpy(err.message, message, sizeof(err.message) - 1);
        err.message[sizeof(err.message) - 1] = 0;
    } else {
        err.message[0] = 0;
    }
}

// Copy a JSON string into a fixed-size dest with truncation.
bool copy_str(JsonVariantConst src, char *dst, size_t cap) {
    if (cap == 0) return false;
    const char *s = src.as<const char *>();
    if (!s) { dst[0] = 0; return false; }
    strncpy(dst, s, cap - 1);
    dst[cap - 1] = 0;
    return true;
}

// "true" iff the variant's `match` block matches the device geometry.
bool variant_matches(JsonObjectConst match, uint16_t w, uint16_t h) {
    if (!match["display"].is<JsonObjectConst>()) return false;
    JsonObjectConst d = match["display"].as<JsonObjectConst>();
    if (d["width"].is<uint16_t>() && d["width"].as<uint16_t>() != w) return false;
    if (d["height"].is<uint16_t>() && d["height"].as<uint16_t>() != h) return false;
    return true;
}

}  // namespace

WidgetType widget_type_from_string(const char *s) {
    if (!s) return WidgetType::Unknown;
    if (!strcmp(s, "numeric"))   return WidgetType::Numeric;
    if (!strcmp(s, "text"))      return WidgetType::Text;
    if (!strcmp(s, "gauge"))     return WidgetType::Gauge;
    if (!strcmp(s, "compass"))   return WidgetType::Compass;
    if (!strcmp(s, "windRose"))  return WidgetType::WindRose;
    if (!strcmp(s, "trend"))     return WidgetType::Trend;
    if (!strcmp(s, "bar"))       return WidgetType::Bar;
    if (!strcmp(s, "button"))    return WidgetType::Button;
    if (!strcmp(s, "autopilot")) return WidgetType::Autopilot;
    return WidgetType::Unknown;
}

const char *widget_type_to_string(WidgetType t) {
    switch (t) {
        case WidgetType::Numeric:   return "numeric";
        case WidgetType::Text:      return "text";
        case WidgetType::Gauge:     return "gauge";
        case WidgetType::Compass:   return "compass";
        case WidgetType::WindRose:  return "windRose";
        case WidgetType::Trend:     return "trend";
        case WidgetType::Bar:       return "bar";
        case WidgetType::Button:    return "button";
        case WidgetType::Autopilot: return "autopilot";
        case WidgetType::Unknown:   return "unknown";
    }
    return "?";
}

const char *parse_code_to_string(ParseCode c) {
    switch (c) {
        case ParseCode::Ok:                    return "ok";
        case ParseCode::InvalidProtocol:       return "invalid_protocol";
        case ParseCode::WrongDevice:           return "wrong_device";
        case ParseCode::DisplayMismatch:       return "display_mismatch";
        case ParseCode::UnsupportedLayoutType: return "unsupported_layout_type";
        case ParseCode::UnsupportedWidgetType: return "unsupported_widget_type";
        case ParseCode::MissingWidget:         return "missing_widget";
        case ParseCode::InvalidFontSize:       return "invalid_font_size";
        case ParseCode::TooManyWidgets:        return "too_many_widgets";
        case ParseCode::TooManyScreens:        return "too_many_screens";
        case ParseCode::TooManyTiles:          return "too_many_tiles";
        case ParseCode::InvalidPath:           return "invalid_path";
        case ParseCode::InvalidAction:         return "invalid_action";
        case ParseCode::OutOfMemory:           return "out_of_memory";
        case ParseCode::BadJson:               return "bad_json";
    }
    return "?";
}

bool parse(JsonObjectConst cfg, uint16_t device_width, uint16_t device_height,
           RenderPlan &out, ParseError &err) {
    // CLAUDE.md memory trap: `out = RenderPlan{}` creates a ~12 KB
    // temporary on the stack which overflows the FreeRTOS worker stack
    // (canary trips, device reboots). memset clears in place.
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    out.display_width = device_width;
    out.display_height = device_height;

    // --- display: must match this device's geometry exactly when
    // present. Plugin may also send `selectedVariant` separately.
    if (cfg["display"].is<JsonObjectConst>()) {
        JsonObjectConst d = cfg["display"].as<JsonObjectConst>();
        if (d["width"].is<uint16_t>() &&
            d["width"].as<uint16_t>() != device_width) {
            set_err(err, ParseCode::DisplayMismatch, "display.width",
                    "config display.width != device");
            return false;
        }
        if (d["height"].is<uint16_t>() &&
            d["height"].as<uint16_t>() != device_height) {
            set_err(err, ParseCode::DisplayMismatch, "display.height",
                    "config display.height != device");
            return false;
        }
        copy_str(d["selectedVariant"], out.layout_variant,
                 sizeof(out.layout_variant));
    }

    // --- widgets block ---
    if (cfg["widgets"].is<JsonObjectConst>()) {
        JsonObjectConst w = cfg["widgets"].as<JsonObjectConst>();
        copy_str(w["variant"], out.widget_variant, sizeof(out.widget_variant));

        // defaults
        if (w["defaults"].is<JsonObjectConst>()) {
            JsonObjectConst dd = w["defaults"].as<JsonObjectConst>();
            out.defaults.font_size       = dd["fontSize"]      | 0;
            out.defaults.label_font_size = dd["labelFontSize"] | 0;
            out.defaults.value_font_size = dd["valueFontSize"] | 0;
            out.defaults.unit_font_size  = dd["unitFontSize"]  | 0;
        }

        if (w["items"].is<JsonObjectConst>()) {
            JsonObjectConst items = w["items"].as<JsonObjectConst>();
            for (JsonPairConst kv : items) {
                if (out.widget_count >= MAX_WIDGETS) {
                    set_err(err, ParseCode::TooManyWidgets, "widgets.items",
                            "MAX_WIDGETS exceeded");
                    return false;
                }
                WidgetDef &wd = out.widgets[out.widget_count];
                strncpy(wd.id, kv.key().c_str(), sizeof(wd.id) - 1);
                wd.id[sizeof(wd.id) - 1] = 0;

                JsonObjectConst item = kv.value().as<JsonObjectConst>();
                const char *type_str = item["type"] | "";
                wd.type = widget_type_from_string(type_str);
                if (wd.type == WidgetType::Unknown) {
                    char path[64];
                    snprintf(path, sizeof(path), "widgets.items.%s.type", wd.id);
                    set_err(err, ParseCode::UnsupportedWidgetType, path,
                            type_str);
                    return false;
                }
                copy_str(item["title"], wd.title, sizeof(wd.title));
                copy_str(item["path"],  wd.path,  sizeof(wd.path));
                copy_str(item["unit"],  wd.unit,  sizeof(wd.unit));
                wd.precision = item["precision"] | 0;
                wd.style.font_size       = item["fontSize"]      | 0;
                wd.style.label_font_size = item["labelFontSize"] | 0;
                wd.style.value_font_size = item["valueFontSize"] | 0;
                wd.style.unit_font_size  = item["unitFontSize"]  | 0;
                if (item["min"].is<double>()) wd.min = item["min"].as<double>();
                if (item["max"].is<double>()) wd.max = item["max"].as<double>();
                out.widget_count++;
            }
        }
    }

    // --- layout block ---
    if (cfg["layout"].is<JsonObjectConst>()) {
        JsonObjectConst lo = cfg["layout"].as<JsonObjectConst>();
        // variants[] vs single
        JsonArrayConst screens;
        if (lo["screens"].is<JsonArrayConst>()) {
            screens = lo["screens"].as<JsonArrayConst>();
        } else if (lo["variants"].is<JsonArrayConst>()) {
            // Pick the first matching variant per spec §Variant Matching.
            for (JsonObjectConst v : lo["variants"].as<JsonArrayConst>()) {
                if (variant_matches(v["match"].as<JsonObjectConst>(),
                                    device_width, device_height)) {
                    if (v["screens"].is<JsonArrayConst>()) {
                        screens = v["screens"].as<JsonArrayConst>();
                        copy_str(v["id"], out.layout_variant,
                                 sizeof(out.layout_variant));
                        break;
                    }
                }
            }
            if (screens.isNull()) {
                set_err(err, ParseCode::DisplayMismatch, "layout.variants",
                        "no variant matched this device's display");
                return false;
            }
        }

        if (!screens.isNull()) {
            for (JsonObjectConst sc : screens) {
                if (out.screen_count >= MAX_SCREENS) {
                    set_err(err, ParseCode::TooManyScreens, "layout.screens",
                            "MAX_SCREENS exceeded");
                    return false;
                }
                ScreenPlan &sp = out.screens[out.screen_count];
                copy_str(sc["id"], sp.id, sizeof(sp.id));

                const char *layout_type = sc["type"] | "grid";
                if (strcmp(layout_type, "grid") != 0) {
                    char path[64];
                    snprintf(path, sizeof(path), "layout.screens[%u].type",
                             (unsigned)out.screen_count);
                    set_err(err, ParseCode::UnsupportedLayoutType, path,
                            layout_type);
                    return false;
                }

                if (sc["tiles"].is<JsonArrayConst>()) {
                    for (JsonObjectConst tl : sc["tiles"].as<JsonArrayConst>()) {
                        if (sp.tile_count >= MAX_TILES_PER_SCREEN) {
                            char path[64];
                            snprintf(path, sizeof(path),
                                     "layout.screens[%u].tiles",
                                     (unsigned)out.screen_count);
                            set_err(err, ParseCode::TooManyTiles, path,
                                    "MAX_TILES_PER_SCREEN exceeded");
                            return false;
                        }
                        LayoutTile &lt = sp.tiles[sp.tile_count];
                        copy_str(tl["widget"], lt.widget_id,
                                 sizeof(lt.widget_id));
                        // Verify the widget id exists in the widgets[]
                        // table - missing reference is a hard reject.
                        bool found = false;
                        for (uint8_t i = 0; i < out.widget_count; ++i) {
                            if (strcmp(out.widgets[i].id, lt.widget_id) == 0) {
                                found = true; break;
                            }
                        }
                        if (!found) {
                            char path[80];
                            snprintf(path, sizeof(path),
                                     "layout.screens[%u].tiles[%u].widget",
                                     (unsigned)out.screen_count,
                                     (unsigned)sp.tile_count);
                            set_err(err, ParseCode::MissingWidget, path,
                                    lt.widget_id);
                            return false;
                        }
                        if (tl["area"].is<JsonObjectConst>()) {
                            JsonObjectConst a = tl["area"].as<JsonObjectConst>();
                            lt.col      = a["col"]     | 0;
                            lt.row      = a["row"]     | 0;
                            lt.col_span = a["colSpan"] | 1;
                            lt.row_span = a["rowSpan"] | 1;
                        }
                        sp.tile_count++;
                    }
                }
                out.screen_count++;
            }
        }
    }

    return true;
}

}  // namespace manager_config
