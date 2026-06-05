#include "screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Steering screen delegated to the QuadGrid template. Matches the editor's
// `steeringScreen()` preset:
//   compass HDG (BTW marker) | numeric XTE
//   numeric VMG               | numeric BTW
// Replaces the 270-line hand-built rotating-ring implementation that read
// as "too small / not a compass" on the device panel.

namespace ui::steering {

static lv_obj_t *s_root = nullptr;

static const ui::layouts::MetricBinding s_tiles[] = {
    // Compass: heading with BTW shown as target bearing in extras.
    {"hdg",
     "HDG",
     "",
     ui::layouts::MetricSource::HDG_deg,
     0x57c7d8 /*accent*/,
     nullptr,
     1,
     {{"BTW", ui::layouts::MetricSource::BTW_deg}},
     ui::layouts::WidgetKind::Compass},
    {"xte",
     "XTE",
     "m",
     ui::layouts::MetricSource::XTE,
     0xffb84d /*warn*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"vmg",
     "VMG",
     "kn",
     ui::layouts::MetricSource::VMG_kn,
     0x39d98a /*good*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"btw",
     "BTW",
     "",
     ui::layouts::MetricSource::BTW_deg,
     0x52736f /*grid*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "steering",
    "Steering",
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

}  // namespace ui::steering
