#include "ui_screens.h"

#include "net.h"

namespace ui {

static Screen s_screens[MAX_SCREENS];
static size_t s_count = 0;
static int s_index = 0;

void register_screen(const Screen &s) {
    if (s_count >= MAX_SCREENS) return;
    s_screens[s_count++] = s;
    if (s_count == 1) {
        lv_screen_load(s.root);
        s_index = 0;
    }
}

void show(int index) {
    if (s_count == 0) return;
    if (index < 0) index = 0;
    if (index >= (int)s_count) index = (int)s_count - 1;
    if (lv_screen_active() == s_screens[index].root && index == s_index) return;
    s_index = index;
    lv_screen_load(s_screens[s_index].root);
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

// Cycle forward to the next non-hidden screen.
void next() {
    if (s_count == 0) return;
    for (int step = 1; step <= (int)s_count; ++step) {
        int idx = (s_index + step) % (int)s_count;
        if (!s_screens[idx].hidden) {
            show(idx);
            return;
        }
    }
}

// Cycle backward to the previous non-hidden screen.
void prev() {
    if (s_count == 0) return;
    for (int step = 1; step <= (int)s_count; ++step) {
        int idx = (s_index + (int)s_count - step) % (int)s_count;
        if (!s_screens[idx].hidden) {
            show(idx);
            return;
        }
    }
}

int current_index() { return s_index; }
const char *current_id() { return s_count == 0 ? "" : s_screens[s_index].id; }
const char *current_title() { return s_count == 0 ? "" : s_screens[s_index].title; }
size_t screen_count() { return s_count; }
bool is_hidden(int index) {
    if (index < 0 || index >= (int)s_count) return true;
    return s_screens[index].hidden;
}

bool screen_info(int index, const char **out_id, const char **out_title, bool *out_hidden) {
    if (index < 0 || index >= (int)s_count) return false;
    if (out_id) *out_id = s_screens[index].id;
    if (out_title) *out_title = s_screens[index].title;
    if (out_hidden) *out_hidden = s_screens[index].hidden;
    return true;
}

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
