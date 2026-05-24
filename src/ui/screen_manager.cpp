#include "ui_screens.h"

#include "net.h"

namespace ui {

static Screen s_screens[MAX_SCREENS];
static size_t s_count = 0;
static int s_index = 0;

static void apply_visibility() {
    for (size_t i = 0; i < s_count; ++i) {
        if ((int)i == s_index)
            lv_obj_clear_flag(s_screens[i].root, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_screens[i].root, LV_OBJ_FLAG_HIDDEN);
    }
}

void register_screen(const Screen &s) {
    if (s_count >= MAX_SCREENS) return;
    s_screens[s_count++] = s;
    // Initial state: only index 0 visible.
    if (s_count == 1)
        lv_obj_clear_flag(s.root, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s.root, LV_OBJ_FLAG_HIDDEN);
}

void show(int index) {
    if (s_count == 0) return;
    if (index < 0) index = 0;
    if (index >= (int)s_count) index = (int)s_count - 1;
    if (index == s_index) return;
    s_index = index;
    apply_visibility();
    net::logf("[ui] screen -> %d (%s)", s_index, s_screens[s_index].id);
}

bool show_by_id(const char *id) {
    if (!id) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_screens[i].id, id) == 0) {
            show((int)i);
            return true;
        }
    }
    return false;
}

void next() {
    if (s_count == 0) return;
    show((s_index + 1) % (int)s_count);
}

void prev() {
    if (s_count == 0) return;
    show((s_index + (int)s_count - 1) % (int)s_count);
}

int current_index() { return s_index; }
const char *current_id() { return s_count == 0 ? "" : s_screens[s_index].id; }
size_t screen_count() { return s_count; }

void refresh_current() {
    if (s_count == 0) return;
    if (s_screens[s_index].refresh) s_screens[s_index].refresh();
}

void log_state() {
    net::logf("[ui] %u screens, current=%d (%s)", (unsigned)s_count, s_index,
              s_count ? s_screens[s_index].id : "-");
    for (size_t i = 0; i < s_count; ++i) {
        net::logf("  [%u] %-12s  %s", (unsigned)i, s_screens[i].id, s_screens[i].title);
    }
}

}  // namespace ui
