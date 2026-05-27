#include "screens.h"
#include "ui_theme.h"
#include "ui_layouts.h"
#include "board_pins.h"
#include "signalk.h"

// Demonstration of the quad_grid template (docs/specs/11). Four tiles
// bound to live SK metrics with tap-to-detail routing. Registered as
// hidden screen "demo_grid"; reachable via console command
// `screen demo_grid`.

namespace ui::demo_grid {

static lv_obj_t *s_root = nullptr;

static const ui::layouts::MetricBinding s_tiles[] = {
    {"wind",    "WIND",   "kn",  ui::layouts::MetricSource::AWS_kn,
     0xf6a21a, "wind"},
    {"speed",   "SPEED",  "kn",  ui::layouts::MetricSource::SOG_kn,
     0x57c7d8, "nav"},
    {"depth",   "DEPTH",  "m",   ui::layouts::MetricSource::Depth_m,
     0x39d98a, "depth"},
    {"battery", "BATT",   "V",   ui::layouts::MetricSource::BatteryV,
     0xff4058, "status"},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "demo_grid",
    "Demo Grid",
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

}  // namespace ui::demo_grid
