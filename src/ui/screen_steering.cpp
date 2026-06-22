#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "ui_layouts.h"
#include "signalk.h"
#include "net.h"
#include "autopilot.h"
#include "board_pins.h"

#include <stdint.h>

// Steering screen delegated to the QuadGrid template. Matches the editor's
// `steeringScreen()` preset:
//   compass HDG (BTW marker) | numeric XTE
//   numeric VMG               | numeric RUDDER
// Replaces the 270-line hand-built rotating-ring implementation that read
// as "too small / not a compass" on the device panel.
//
// A compact course-adjust button row [-10][-1][+1][+10] is overlaid along the
// very bottom edge of the screen (centered) after the template is built; each
// button PUTs an autopilot heading delta via the (thread-safe) net worker.

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
     "nm",
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

// ---- course-adjust overlay -------------------------------------------------
// Four small buttons [-10][-1][+1][+10] PUTting a heading delta through the net
// worker. Runs on the LVGL task; the callback keeps no large stack scratch.
static void on_nudge(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    ::autopilot::adjust_heading_deg(delta);
}

// One course-adjust chip (small accent button overlaid on the QuadGrid).
static lv_obj_t *nudge_btn(lv_obj_t *parent, const char *txt, int delta) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 56, 36);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(theme.panel_bot), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(b, ui::chrome::panel_border, 0);
    lv_obj_set_style_radius(b, ui::chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(theme.fg), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, on_nudge, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)delta);
    return b;
}

// Build the [-10][-1][+1][+10] row centered along the very bottom edge. The
// QuadGrid value tiles center their content, leaving the bottom strip clear, so
// a compact 36px row sits in that gap without covering the readouts.
static void build_course_row(lv_obj_t *parent) {
    static const struct {
        const char *txt;
        int delta;
    } btns[4] = {{"-10", -10}, {"-1", -1}, {"+1", +1}, {"+10", +10}};
    int gap = 6;
    int bw = 56;
    int total = 4 * bw + 3 * gap;
    int x0 = (LCD_W - total) / 2;
    int y = LCD_H - 36 - 6;  // bottom edge, inside the small QuadGrid margin
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b = nudge_btn(parent, btns[i].txt, btns[i].delta);
        lv_obj_set_pos(b, x0 + i * (bw + gap), y);
    }
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = ui::layouts::create(parent, s_spec);
    ui::set_screen_collect_paths(s_spec.screen_id, collect_paths);
    if (s_root) build_course_row(s_root);
    return s_root;
}

void refresh() {
    if (!s_root) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update(s_root, s_spec, d);
}

}  // namespace ui::steering
