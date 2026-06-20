#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "board_pins.h"

// Nav screen delegated to the QuadGrid template with widget kinds that
// mirror the editor's `fullscreenNav()` preset:
//   compass HDG (CTS in extras[0]) / numeric SOG / numeric COG / text POS
// Hand-built version had ~165 lines of bespoke LVGL; the template path
// renders the same data with one MetricBinding table.

namespace ui::nav {

static lv_obj_t *s_root = nullptr;

static const ui::layouts::MetricBinding s_tiles[] = {
    // Compass: heading with CTS (course-to-steer) as the secondary reference
    // angle. COG was previously the secondary here AND a dedicated tile below
    // (tile 2) -- a duplicate readout, and the "COG" bottom line crowded the
    // ring's W cardinal. CTS is the steering reference (not otherwise shown on
    // this screen), so it removes the duplication and earns the bottom slot.
    {"hdg",
     "HDG",
     "",
     ui::layouts::MetricSource::HDG_deg,
     0x57c7d8 /*accent*/,
     nullptr,
     1,
     {{"CTS", ui::layouts::MetricSource::CTS_deg}},
     ui::layouts::WidgetKind::Compass},
    {"sog",
     "SOG",
     "kn",
     ui::layouts::MetricSource::SOG_kn,
     0x57c7d8 /*accent*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"cog",
     "COG",
     "",
     ui::layouts::MetricSource::COG_deg,
     0x52736f /*grid*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Numeric},
    {"pos",
     "POS",
     "",
     ui::layouts::MetricSource::Position,
     0x39d98a /*good*/,
     nullptr,
     0,
     {},
     ui::layouts::WidgetKind::Text},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "nav", "Nav", ui::layouts::TemplateId::QuadGrid, s_tiles, sizeof(s_tiles) / sizeof(s_tiles[0]),
    0,
};

// Slice 3: the paths this screen subscribes (derived from its MetricBinding
// table). Registered below so the subscription manager picks them up on show.
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

}  // namespace ui::nav
