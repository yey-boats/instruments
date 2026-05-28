#include "manager_screens.h"

#include <esp_heap_caps.h>

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "widget_registry.h"

namespace manager_screens {

namespace {

constexpr int MAX_MANAGED_WIDGETS = manager_config::MAX_TILES_PER_SCREEN;

// MOB-safe band per spec 09 (also reported in identity.display.safeArea).
constexpr int SAFE_TOP = 62;
constexpr int MARGIN = 8;

lv_obj_t *s_root = nullptr;
widget_registry::Widget *s_widgets[MAX_MANAGED_WIDGETS] = {nullptr};
uint8_t s_widget_count = 0;
bool s_applied = false;

const manager_config::WidgetDef *find_widget(
        const manager_config::RenderPlan &plan, const char *id) {
    for (uint8_t i = 0; i < plan.widget_count; ++i) {
        if (strcmp(plan.widgets[i].id, id) == 0) return &plan.widgets[i];
    }
    return nullptr;
}

void refresh_cb() { refresh(); }

}  // namespace

bool apply(const manager_config::RenderPlan &plan) {
    if (s_applied) {
        net::logf("[mgr-screens] already applied; ignoring re-apply");
        return false;
    }
    if (plan.screen_count == 0) {
        net::logf("[mgr-screens] no screens in plan");
        return false;
    }
    const manager_config::ScreenPlan &sc = plan.screens[0];

    // Compute grid dimensions from the max tile extents.
    uint8_t cols = 1, rows = 1;
    for (uint8_t i = 0; i < sc.tile_count; ++i) {
        const auto &t = sc.tiles[i];
        uint8_t span_c = t.col + (t.col_span ? t.col_span : 1);
        uint8_t span_r = t.row + (t.row_span ? t.row_span : 1);
        if (span_c > cols) cols = span_c;
        if (span_r > rows) rows = span_r;
    }
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int avail_w = LCD_W - 2 * MARGIN;
    int avail_h = LCD_H - SAFE_TOP - MARGIN;
    int cell_w = avail_w / cols;
    int cell_h = avail_h / rows;

    // Build the screen root.
    s_root = lv_obj_create(NULL);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(ui::theme.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Title from the variant name.
    if (plan.layout_variant[0]) {
        lv_obj_t *title = lv_label_create(s_root);
        lv_label_set_text(title, plan.layout_variant);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(ui::theme.fg_dim), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    }

    // Build widgets.
    s_widget_count = 0;
    for (uint8_t i = 0; i < sc.tile_count && s_widget_count < MAX_MANAGED_WIDGETS; ++i) {
        const auto &t = sc.tiles[i];
        const auto *def = find_widget(plan, t.widget_id);
        if (!def) continue;  // manager_config::parse already rejects missing
        int x = MARGIN + t.col * cell_w;
        int y = SAFE_TOP + t.row * cell_h;
        int w = cell_w * (t.col_span ? t.col_span : 1) - 4;
        int h = cell_h * (t.row_span ? t.row_span : 1) - 4;
        s_widgets[s_widget_count] = widget_registry::create(
            s_root, x, y, w, h, *def, plan.defaults);
        if (s_widgets[s_widget_count]) s_widget_count++;
    }

    ui::Screen screen = {
        "mgr_main",
        sc.id[0] ? sc.id : "Managed",
        s_root,
        refresh_cb,
        false,  // visible in the carousel
    };
    ui::register_screen(screen);
    s_applied = true;
    net::logf("[mgr-screens] applied: variant=%s grid=%ux%u widgets=%u",
              plan.layout_variant[0] ? plan.layout_variant : "(none)",
              (unsigned)cols, (unsigned)rows, (unsigned)s_widget_count);
    return true;
}

void refresh() {
    if (!s_applied || s_widget_count == 0) return;
    sk::Data d;
    sk::copyData(d);
    for (uint8_t i = 0; i < s_widget_count; ++i) {
        if (s_widgets[i]) widget_registry::update(*s_widgets[i], d);
    }
}

bool is_applied() { return s_applied; }

}  // namespace manager_screens
