#include "manager_screens.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "board_pins.h"
#include "net.h"
#include "signalk.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "widget_registry.h"

namespace manager_screens {

namespace {

constexpr int MAX_MANAGED_SCREENS = 8;
constexpr int MAX_MANAGED_WIDGETS = manager_config::MAX_TILES_PER_SCREEN;

// MOB-safe band per spec 09 (also reported in identity.display.safeArea).
constexpr int SAFE_TOP = 62;
constexpr int MARGIN = 8;

struct ManagedScreen {
    char id[40];  // "mgr_" + up to 35 chars from plan id
    lv_obj_t *root;
    widget_registry::Widget *widgets[MAX_MANAGED_WIDGETS];
    uint8_t widget_count;
};

ManagedScreen s_screens[MAX_MANAGED_SCREENS] = {};
uint8_t s_screen_count = 0;
bool s_applied = false;

const manager_config::WidgetDef *find_widget(const manager_config::RenderPlan &plan,
                                             const char *id) {
    for (uint8_t i = 0; i < plan.widget_count; ++i) {
        if (strcmp(plan.widgets[i].id, id) == 0) return &plan.widgets[i];
    }
    return nullptr;
}

// Per-active-screen refresh dispatcher. ui::refresh_current() invokes
// the registered Screen.refresh callback only for the currently visible
// screen, but it doesn't tell us which one - look up by id.
void refresh_cb() {
    if (!s_applied || s_screen_count == 0) return;
    const char *cur = ui::current_id();
    if (!cur) return;
    for (uint8_t i = 0; i < s_screen_count; ++i) {
        if (strcmp(s_screens[i].id, cur) != 0) continue;
        sk::Data d;
        sk::copyData(d);
        for (uint8_t w = 0; w < s_screens[i].widget_count; ++w) {
            if (s_screens[i].widgets[w]) {
                widget_registry::update(*s_screens[i].widgets[w], d);
            }
        }
        return;
    }
}

bool build_screen(const manager_config::RenderPlan &plan, const manager_config::ScreenPlan &sc,
                  uint8_t plan_index, ManagedScreen &out) {
    // Compose the managed id. Plan ids may collide with built-in
    // screens, so prefix with "mgr_" to keep the carousel ids unique.
    if (sc.id[0]) {
        snprintf(out.id, sizeof(out.id), "mgr_%s", sc.id);
    } else {
        snprintf(out.id, sizeof(out.id), "mgr_s%u", (unsigned)plan_index);
    }
    out.id[sizeof(out.id) - 1] = '\0';

    // Grid sizing from the max tile extents.
    uint8_t cols = 1, rows = 1;
    for (uint8_t i = 0; i < sc.tile_count; ++i) {
        const auto &t = sc.tiles[i];
        uint8_t span_c = t.col + (t.col_span ? t.col_span : 1);
        uint8_t span_r = t.row + (t.row_span ? t.row_span : 1);
        if (span_c > cols) cols = span_c;
        if (span_r > rows) rows = span_r;
    }
    int avail_w = LCD_W - 2 * MARGIN;
    int avail_h = LCD_H - SAFE_TOP - MARGIN;
    int cell_w = avail_w / cols;
    int cell_h = avail_h / rows;

    out.root = lv_obj_create(NULL);
    lv_obj_set_size(out.root, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(out.root, lv_color_hex(ui::theme.bg), 0);
    lv_obj_set_style_bg_opa(out.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(out.root, 0, 0);
    lv_obj_set_style_pad_all(out.root, 0, 0);
    lv_obj_clear_flag(out.root, LV_OBJ_FLAG_SCROLLABLE);

    if (plan.layout_variant[0]) {
        lv_obj_t *title = lv_label_create(out.root);
        lv_label_set_text(title, plan.layout_variant);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(ui::theme.fg_dim), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    }

    out.widget_count = 0;
    for (uint8_t i = 0; i < sc.tile_count && out.widget_count < MAX_MANAGED_WIDGETS; ++i) {
        const auto &t = sc.tiles[i];
        const auto *def = find_widget(plan, t.widget_id);
        if (!def) continue;  // parse() already rejects missing refs
        int x = MARGIN + t.col * cell_w;
        int y = SAFE_TOP + t.row * cell_h;
        int w = cell_w * (t.col_span ? t.col_span : 1) - 4;
        int h = cell_h * (t.row_span ? t.row_span : 1) - 4;
        out.widgets[out.widget_count] =
            widget_registry::create(out.root, x, y, w, h, *def, plan.defaults);
        if (out.widgets[out.widget_count]) out.widget_count++;
    }
    return true;
}

void free_widget_handles(ManagedScreen &screen) {
    for (uint8_t i = 0; i < screen.widget_count; ++i) {
        widget_registry::destroy(screen.widgets[i]);
        screen.widgets[i] = nullptr;
    }
    screen.widget_count = 0;
}

}  // namespace

bool apply(const manager_config::RenderPlan &plan) {
    if (plan.screen_count == 0) {
        net::logf("[mgr-screens] no screens in plan");
        return false;
    }

    uint8_t limit = plan.screen_count;
    if (limit > MAX_MANAGED_SCREENS) {
        net::logf("[mgr-screens] plan has %u screens, capping to %u", (unsigned)limit,
                  (unsigned)MAX_MANAGED_SCREENS);
        limit = MAX_MANAGED_SCREENS;
    }

    uint8_t previous_count = s_screen_count;
    s_screen_count = 0;
    for (uint8_t i = 0; i < limit; ++i) {
        char replace_id[sizeof(s_screens[s_screen_count].id)];
        strncpy(replace_id, s_screens[s_screen_count].id, sizeof(replace_id) - 1);
        replace_id[sizeof(replace_id) - 1] = 0;
        free_widget_handles(s_screens[s_screen_count]);
        if (!build_screen(plan, plan.screens[i], i, s_screens[s_screen_count])) {
            continue;
        }
        ui::Screen reg = {
            s_screens[s_screen_count].id,
            plan.screens[i].id[0] ? plan.screens[i].id : "Managed",
            s_screens[s_screen_count].root,
            refresh_cb,
            false,  // visible in carousel
        };
        if (s_applied) {
            if (!ui::replace_screen(replace_id, reg)) {
                ui::register_screen(reg);
            }
        } else {
            ui::register_screen(reg);
        }
        net::logf("[mgr-screens] +%s widgets=%u", s_screens[s_screen_count].id,
                  (unsigned)s_screens[s_screen_count].widget_count);
        s_screen_count++;
    }

    for (uint8_t i = s_screen_count; i < previous_count; ++i) {
        if (s_screens[i].id[0]) ui::set_screen_hidden(s_screens[i].id, true);
        free_widget_handles(s_screens[i]);
    }

    s_applied = s_screen_count > 0;
    if (s_applied) {
        net::logf("[mgr-screens] applied: variant=%s screens=%u",
                  plan.layout_variant[0] ? plan.layout_variant : "(none)",
                  (unsigned)s_screen_count);
    }
    return s_applied;
}

void refresh() {
    refresh_cb();
}

bool is_applied() {
    return s_applied;
}

uint8_t managed_count() {
    return s_screen_count;
}

}  // namespace manager_screens
