#include "screens.h"
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

static const ui::layouts::MetricBinding s_tiles[] = {
    // WIND: AWS hero, AWA below
    {"wind",
     "WIND",
     "kn",
     ui::layouts::MetricSource::AWS_kn,
     0xffb84d /*warn*/,
     "wind",
     1,
     {
         {"AWA", ui::layouts::MetricSource::AWA_deg},
         {},
         {},
         {},
     }},
    // NAV: SOG hero, COG/HDG/position below
    {"nav",
     "NAV",
     "kn",
     ui::layouts::MetricSource::SOG_kn,
     0x57c7d8 /*accent*/,
     "nav",
     3,
     {
         {"COG", ui::layouts::MetricSource::COG_deg},
         {"HDG", ui::layouts::MetricSource::HDG_deg},
         {"", ui::layouts::MetricSource::Position},
     }},
    // DEPTH: depth hero, water temp below
    {"depth",
     "DEPTH",
     "m",
     ui::layouts::MetricSource::Depth_m,
     0x39d98a /*good*/,
     "depth",
     1,
     {
         {"H2O", ui::layouts::MetricSource::WaterTemp_C},
         {},
         {},
         {},
     }},
    // SYSTEM: battery V hero, SOC below
    {"system",
     "SYSTEM",
     "V",
     ui::layouts::MetricSource::BatteryV,
     0x52736f /*grid*/,
     "status",
     1,
     {
         {"SOC", ui::layouts::MetricSource::BatterySOC_pct},
         {},
         {},
         {},
     }},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "dashboard",
    "Dashboard",
    ui::layouts::TemplateId::QuadGrid,
    s_tiles,
    sizeof(s_tiles) / sizeof(s_tiles[0]),
    0,
};

lv_obj_t *build(lv_obj_t *parent) {
    s_root = ui::layouts::create(parent, s_spec);
    return s_root;
}

void refresh() {
    if (!s_root) return;
    sk::Data d;
    sk::copyData(d);
    ui::layouts::update(s_root, s_spec, d);
}

}  // namespace ui::dashboard
