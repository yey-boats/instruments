#include "capabilities.h"

#include "font_resolver.h"
#include "layout.h"
#include "marker_math.h"

namespace capabilities {

namespace {

// One view type entry: { paths:[...], attrs:[...], zoom:[...] }.
void view_type(JsonObject vt, const char *const *paths, int np, const char *const *attrs, int na,
               const char *const *zoom, int nz) {
    JsonArray pa = vt["paths"].to<JsonArray>();
    for (int i = 0; i < np; ++i)
        pa.add(paths[i]);
    JsonArray aa = vt["attrs"].to<JsonArray>();
    for (int i = 0; i < na; ++i)
        aa.add(attrs[i]);
    JsonArray za = vt["zoom"].to<JsonArray>();
    for (int i = 0; i < nz; ++i)
        za.add(zoom[i]);
}

void unit_family(JsonObject units, const char *name, const char *const *vals, int n) {
    JsonArray a = units[name].to<JsonArray>();
    for (int i = 0; i < n; ++i)
        a.add(vals[i]);
}

}  // namespace

void build_manifest(JsonObject out) {
    out["version"] = MANIFEST_VERSION;

    JsonObject vts = out["viewTypes"].to<JsonObject>();
    static const char *P_VALUE[] = {"value"};
    static const char *P_VALUE_DIR[] = {"value", "dir"};
    static const char *P_VALUE_DIROPT[] = {"value", "dir?"};
    static const char *ZOOM_AUTO[] = {"auto"};
    static const char *ZOOM_AUTO_REF[] = {"auto", "screenRef"};
    static const char *ZOOM_REF[] = {"screenRef"};

    static const char *A_NUM[] = {"title", "format", "size", "unit", "color"};
    static const char *A_COMPASS[] = {"title", "size", "color"};
    static const char *A_GAUGE[] = {"title", "size", "unit", "color", "range", "zones"};
    static const char *A_TREND[] = {"title", "size", "unit", "color"};
    static const char *A_TEXT[] = {"title", "size", "color"};

    view_type(vts["numeric"].to<JsonObject>(), P_VALUE, 1, A_NUM, 5, ZOOM_AUTO, 1);
    view_type(vts["compass"].to<JsonObject>(), P_VALUE_DIROPT, 2, A_COMPASS, 3, ZOOM_AUTO_REF, 2);
    view_type(vts["windCircle"].to<JsonObject>(), P_VALUE_DIR, 2, A_NUM, 5, ZOOM_AUTO_REF, 2);
    view_type(vts["gauge"].to<JsonObject>(), P_VALUE, 1, A_GAUGE, 6, ZOOM_AUTO, 1);
    view_type(vts["bar"].to<JsonObject>(), P_VALUE, 1, A_GAUGE, 6, ZOOM_AUTO, 1);
    view_type(vts["trend"].to<JsonObject>(), P_VALUE, 1, A_TREND, 4, ZOOM_AUTO, 1);
    view_type(vts["text"].to<JsonObject>(), P_VALUE, 1, A_TEXT, 3, nullptr, 0);
    {
        JsonObject control = vts["control"].to<JsonObject>();
        view_type(control, P_VALUE, 1, A_TEXT, 3, ZOOM_REF, 1);
        JsonArray cc = control["controls"].to<JsonArray>();
        cc.add("autopilot");
    }

    JsonArray fs = out["fontSizes"].to<JsonArray>();
    constexpr int n_fonts =
        (int)(sizeof(font_resolver::DEFAULT_SIZES) / sizeof(font_resolver::DEFAULT_SIZES[0]));
    for (int i = 0; i < n_fonts; ++i)
        fs.add((int)font_resolver::DEFAULT_SIZES[i]);

    JsonObject units = out["units"].to<JsonObject>();
    static const char *U_SPEED[] = {"kn", "m/s"};
    static const char *U_ANGLE[] = {"deg"};
    static const char *U_DEPTH[] = {"m", "ft"};
    static const char *U_TEMP[] = {"C", "F"};
    static const char *U_RATIO[] = {"%"};
    static const char *U_VOLT[] = {"V"};
    unit_family(units, "speed", U_SPEED, 2);
    unit_family(units, "angle", U_ANGLE, 1);
    unit_family(units, "depth", U_DEPTH, 2);
    unit_family(units, "temp", U_TEMP, 2);
    unit_family(units, "ratio", U_RATIO, 1);
    unit_family(units, "voltage", U_VOLT, 1);

    out["maxViews"] = (int)layout::MAX_SCREENS;
    out["maxTilesPerScreen"] = (int)layout::MAX_TILES_PER_SCREEN;
    out["maxMarkersPerDial"] = (int)ui::kMaxMarkersPerDial;
    out["paths"] = "open";  // generic path store renders any path (Slice 1)

    // Marker glyph token set the firmware can render, in marker_math's canonical
    // order. Single source of truth so firmware, manifest, and editor stay in
    // lockstep (see marker_math.h).
    JsonArray glyphs = out["glyphs"].to<JsonArray>();
    for (uint8_t i = 0; i < (uint8_t)ui::GlyphId::COUNT; ++i)
        glyphs.add(ui::glyph_to_token((ui::GlyphId)i));

    JsonArray controls = out["controls"].to<JsonArray>();
    controls.add("autopilot");

    JsonArray themes = out["themes"].to<JsonArray>();
    themes.add("day");
    themes.add("night");
    themes.add("high-contrast");
}

}  // namespace capabilities
