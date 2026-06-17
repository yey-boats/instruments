#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Route screen delegated to the QuadGrid template. Matches the editor's
// `routeScreen()` preset: numeric DTW / numeric BTW / numeric XTE /
// numeric VMG.

namespace ui::route {

static lv_obj_t *s_root = nullptr;

static const ui::layouts::MetricBinding s_tiles[] = {
    {"dtw",
     "DTW",
     "nm",
     ui::layouts::MetricSource::DTW,
     0x57c7d8 /*accent*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"btw",
     "BTW",
     "",
     ui::layouts::MetricSource::BTW_deg,
     0x39d98a /*good*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
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
     0x52736f /*grid*/,
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

}  // namespace ui::route
