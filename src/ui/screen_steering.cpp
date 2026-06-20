#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Steering screen delegated to the QuadGrid template. Matches the editor's
// `steeringScreen()` preset:
//   compass HDG (BTW marker) | numeric XTE
//   numeric VMG               | numeric RUDDER
// Replaces the 270-line hand-built rotating-ring implementation that read
// as "too small / not a compass" on the device panel.

namespace ui::steering {

static lv_obj_t *s_root = nullptr;

static const ui::layouts::MetricBinding s_tiles[] = {
    // Compass: heading with CTS (course-to-steer) as secondary — matches
    // the editor's steeringScreen() preset (HDG / CTS).
    {"hdg",
     "HDG / CTS",
     "",
     ui::layouts::MetricSource::HDG_deg,
     0x57c7d8 /*accent*/,
     nullptr,
     1,
     {{"CTS", ui::layouts::MetricSource::CTS_deg}},
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
    {"rudder",
     "RUDDER",
     "deg",
     ui::layouts::MetricSource::Rudder_deg,
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
    sk::Data d;
    sk::copyData(d);
    ui::layouts::update(s_root, s_spec, d);
}

}  // namespace ui::steering
