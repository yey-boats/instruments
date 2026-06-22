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
static const ui::layouts::MetricBinding s_tiles[] = {
    // WIND: wind-rose ring with AWS in center (tap -> wind detail).
    {"wind",
     "WIND",
     "kn",
     ui::layouts::MetricSource::AWS_kn,
     0xffb84d /*warn*/,
     "wind",
     0,
     {},
     ui::layouts::WidgetKind::WindRose},
    // SOG: big numeric primary, accent color (matches editor numeric tile).
    {"sog",
     "SOG",
     "kn",
     ui::layouts::MetricSource::SOG_kn,
     0x57c7d8 /*accent*/,
     "nav",
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    // DEPTH: numeric primary.
    {"depth",
     "DEPTH",
     "m",
     ui::layouts::MetricSource::Depth_m,
     0x39d98a /*good*/,
     "depth",
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    // BATT SOC: horizontal bar (matches editor's .bar preview).
    {"batt",
     "BATT",
     "",
     ui::layouts::MetricSource::BatterySOC_pct,
     0x52736f /*grid*/,
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
