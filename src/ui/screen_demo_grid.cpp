#include "screens.h"
#include "ui_screens.h"
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

// Accents are theme tokens resolved in build() (a theme switch rebuilds every
// screen); raw night-palette literals would render wrong on the other skins.
static ui::layouts::MetricBinding s_tiles[] = {
    {"wind", "WIND", "kn", ui::layouts::MetricSource::AWS_kn, 0 /*warn*/, "wind"},
    {"speed", "SPEED", "kn", ui::layouts::MetricSource::SOG_kn, 0 /*accent*/, "nav"},
    {"depth", "DEPTH", "m", ui::layouts::MetricSource::Depth_m, 0 /*good*/, "depth"},
    {"battery", "BATT", "V", ui::layouts::MetricSource::BatteryV, 0 /*alarm*/, "status"},
};

static const ui::layouts::ScreenVariantSpec s_spec = {
    "demo_grid",
    "Demo Grid",
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
    s_tiles[0].accent = ui::theme.warn;
    s_tiles[1].accent = ui::theme.accent;
    s_tiles[2].accent = ui::theme.good;
    s_tiles[3].accent = ui::theme.alarm;
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

}  // namespace ui::demo_grid
