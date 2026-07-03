#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Dashboard migrated to the quad_grid template per docs/specs/11.
// Four tiles (WIND / NAV / DEPTH / SYSTEM) with multi-row extras
// preserving the data the hand-built version showed. Tap-to-detail
// posts ShowScreen via app::post.
//
// The earlier hand-built version had ~225 lines of LVGL setup +
// refresh; this is ~70 by delegating to the template factory.

namespace ui::dashboard {

static lv_obj_t *s_root = nullptr;

// Tile catalog matches the editor's `dashboardQuad()` preset:
// windRose / numeric SOG / numeric DEPTH / bar BATT_SOC. Widget kinds
// drive the per-tile painter in ui_layouts.cpp so the device render
// approximates the editor's widgetPreview() canvas.
// Tile accents are theme tokens resolved in build() (a theme switch rebuilds
// every screen). Hardcoded night-palette literals here rendered cyan/green on
// the classic / red-night skins — the table must never carry raw 0xRRGGBB.
static ui::layouts::MetricBinding s_tiles[] = {
    // WIND: wind-rose ring with AWS in center (tap -> wind detail).
    // The WindRose painter draws the apparent/true wind-angle chevrons and the
    // secondary AWA text, so the tile MUST subscribe the angle + true-wind paths
    // as well as AWS. extras[] is the ONLY hook collect_paths() reads beyond the
    // primary source (see ui_layouts.cpp::collect_paths): without AWA/TWA/TWS
    // here the per-screen subscription manager never requests
    // environment.wind.angleApparent / angleTrueWater / speedTrue, so once the
    // device settles on the dashboard those paths are unsubscribed and the wind
    // angle reads "--" everywhere (sk::data.awa/twa/tws go NaN). The extras are
    // not rendered as text rows by the WindRose painter (it consumes the values
    // directly from boat::View); they exist purely to declare the subscription.
    {"wind",
     "WIND",
     "kn",
     ui::layouts::MetricSource::AWS_kn,
     0 /*warn: theme token, set in build()*/,
     "wind",
     3,
     {{"AWA", ui::layouts::MetricSource::AWA_deg},
      {"TWS", ui::layouts::MetricSource::TWS_kn},
      {"TWA", ui::layouts::MetricSource::TWA_deg}},
     ui::layouts::WidgetKind::WindRose},
    // SOG: big numeric primary, accent color (matches editor numeric tile).
    {"sog",
     "SOG",
     "kn",
     ui::layouts::MetricSource::SOG_kn,
     0 /*accent: theme token, set in build()*/,
     "nav",
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    // DEPTH: numeric primary.
    {"depth",
     "DEPTH",
     "m",
     ui::layouts::MetricSource::Depth_m,
     0 /*good: theme token, set in build()*/,
     "depth",
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    // BATT SOC: horizontal bar (matches editor's .bar preview).
    {"batt",
     "BATT",
     "",
     ui::layouts::MetricSource::BatterySOC_pct,
     0 /*good: theme token, set in build()*/,
     "status",
     0,
     {},
     ui::layouts::WidgetKind::Bar},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "dashboard",
    "Dashboard",
    ui::layouts::TemplateId::QuadGrid,
    s_tiles,
    sizeof(s_tiles) / sizeof(s_tiles[0]),
    0,
};

static void collect_paths(sk::SubscriptionSet &out) {
    ui::layouts::collect_paths(s_spec, out);
}

lv_obj_t *build(lv_obj_t *parent) {
    // Resolve accents from the LIVE palette (theme switches rebuild screens).
    s_tiles[0].accent = ui::theme.warn;    // WIND rose
    s_tiles[1].accent = ui::theme.accent;  // SOG hero
    s_tiles[2].accent = ui::theme.good;    // DEPTH: safe-water cue
    s_tiles[3].accent = ui::theme.good;    // BATT bar: positive/charge cue
    s_root = ui::layouts::create(parent, s_spec);
    ui::set_screen_collect_paths(s_spec.screen_id, collect_paths);
    return s_root;
}

void refresh() {
    if (!s_root) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update(s_root, s_spec, d);
}

}  // namespace ui::dashboard
