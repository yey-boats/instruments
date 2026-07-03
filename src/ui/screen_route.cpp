#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Route screen delegated to the QuadGrid template. Matches the editor's
// `routeScreen()` preset: numeric DTW / numeric BTW / numeric XTE /
// numeric VMG. CTS (course-to-steer) was missing; the QuadGrid only renders
// 4 tiles, so CTS rides as a secondary line under BTW (both are bearings, and
// CTS is the angle you actually steer to make the leg good against current).

namespace ui::route {

static lv_obj_t *s_root = nullptr;

// Tile accents are theme tokens resolved in build() (a theme switch rebuilds
// every screen). Hardcoded night-palette literals here rendered cyan/green on
// the classic / red-night skins — the table must never carry raw 0xRRGGBB.
static ui::layouts::MetricBinding s_tiles[] = {
    {"dtw",
     "DTW",
     "nm",
     ui::layouts::MetricSource::DTW,
     0 /*accent: theme token, set in build()*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"btw",
     "BTW",
     "",
     ui::layouts::MetricSource::BTW_deg,
     0 /*good: theme token, set in build()*/,
     nullptr,
     1,
     {{"CTS", ui::layouts::MetricSource::CTS_deg}},
     ui::layouts::WidgetKind::Numeric},
    {"xte",
     "XTE",
     "nm",
     ui::layouts::MetricSource::XTE,
     0 /*warn: theme token, set in build()*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"vmg",
     "VMG",
     "kn",
     ui::layouts::MetricSource::VMG_kn,
     0 /*fg: theme token, set in build()*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "route",
    "Route",
    ui::layouts::TemplateId::QuadGrid,
    s_tiles,
    sizeof(s_tiles) / sizeof(s_tiles[0]),
    0,
};

static void collect_paths(sk::SubscriptionSet &out) {
    ui::layouts::collect_paths(s_spec, out);
}

// The QuadGrid Numeric painter anchors extras rows a FIXED 60/76 px below the
// tile top while the value row is CENTERED in the tile: on the tall 1024x600
// tiles the centred numeral slides down INTO the fixed rows and "CTS 061"
// printed on top of the BTW value. Re-anchor every extras label relative to
// the tile CENTRE so the rows track tile height at any resolution: the value
// row (montserrat_48, centred at -28) ends at centre-4, so the first extras
// row starts at centre+12 (8 px clear). Extras are the only montserrat_14
// TOP_MID labels below the caption band in a Numeric tile, so the signature
// match is unambiguous. (Proper home for this is the painter in
// ui_layouts.cpp, which is owned elsewhere right now.)
static void reanchor_extras(lv_obj_t *root) {
    if (!root) return;
    uint32_t nt = lv_obj_get_child_count(root);
    for (uint32_t ti = 0; ti < nt; ++ti) {
        lv_obj_t *tile = lv_obj_get_child(root, ti);
        uint32_t nc = lv_obj_get_child_count(tile);
        int k = 0;
        for (uint32_t ci = 0; ci < nc; ++ci) {
            lv_obj_t *c = lv_obj_get_child(tile, ci);
            if (!lv_obj_check_type(c, &lv_label_class)) continue;
            if (lv_obj_get_style_text_font(c, LV_PART_MAIN) != &lv_font_montserrat_14) continue;
            if (lv_obj_get_style_align(c, LV_PART_MAIN) != LV_ALIGN_TOP_MID) continue;
            if ((int)lv_obj_get_style_y(c, LV_PART_MAIN) < 56) continue;  // caption/unit band
            lv_obj_align(c, LV_ALIGN_CENTER, 0, 12 + k * 20);
            ++k;
        }
    }
}

lv_obj_t *build(lv_obj_t *parent) {
    // Resolve accents from the LIVE palette (theme switches rebuild screens).
    s_tiles[0].accent = ui::theme.accent;  // DTW hero
    s_tiles[1].accent = ui::theme.good;    // BTW: on-course cue
    s_tiles[2].accent = ui::theme.warn;    // XTE: deviation cue
    s_tiles[3].accent = ui::theme.fg;      // VMG: neutral value
    s_root = ui::layouts::create(parent, s_spec);
    reanchor_extras(s_root);
    ui::set_screen_collect_paths(s_spec.screen_id, collect_paths);
    return s_root;
}

void refresh() {
    if (!s_root) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update(s_root, s_spec, d);
}

}  // namespace ui::route
