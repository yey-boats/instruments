#include "ui_screens.h"

#include "net.h"

namespace ui {

static Screen s_screens[MAX_SCREENS];
static size_t s_count = 0;
static int s_index = 0;
static ScreenPostBuildCb s_post_build_cb = nullptr;

void set_post_build_cb(ScreenPostBuildCb cb) {
    s_post_build_cb = cb;
    // Apply to any already-built screens (eager registrations made
    // before main installed the callback get the post-build hook
    // retroactively so gesture handlers etc. land on them too).
    for (size_t i = 0; i < s_count; ++i) {
        if (s_screens[i].root && s_post_build_cb)
            s_post_build_cb(s_screens[i].root, s_screens[i].id);
    }
}

void register_screen(const Screen &s) {
    if (s_count >= MAX_SCREENS) return;
    s_screens[s_count++] = s;
    if (s_count == 1 && s.root) {
        lv_screen_load(s.root);
        s_index = 0;
    }
    if (s.root && s_post_build_cb) s_post_build_cb(s.root, s.id);
}

void register_screen_lazy(const char *id, const char *title, lv_obj_t *(*build_fn)(lv_obj_t *),
                          void (*refresh_fn)(), bool hidden) {
    if (s_count >= MAX_SCREENS || !build_fn) return;
    Screen s = {};
    s.id = id;
    s.title = title;
    s.root = nullptr;
    s.refresh = refresh_fn;
    s.hidden = hidden;
    s.build_fn = build_fn;
    s_screens[s_count++] = s;
    // Don't lv_screen_load on registration - first show() builds + loads.
}

// Build the screen's LVGL tree on demand and cache the root. No-op if
// already built or if this is an eager-registered screen (build_fn=NULL).
// Fires the post-build callback so main.cpp can attach the gesture
// handler (lazy screens otherwise miss the boot-time attachment pass).
static void ensure_built(size_t i) {
    if (i >= s_count) return;
    if (s_screens[i].root) return;
    if (!s_screens[i].build_fn) return;
    s_screens[i].root = s_screens[i].build_fn(nullptr);
    net::logf("[ui] lazy-built screen %s root=%p", s_screens[i].id, s_screens[i].root);
    if (s_screens[i].root && s_post_build_cb) s_post_build_cb(s_screens[i].root, s_screens[i].id);
}

bool replace_screen(const char *id, const Screen &s) {
    if (!id) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_screens[i].id, id) != 0) continue;
        bool active = (int)i == s_index;
        lv_obj_t *old_root = s_screens[i].root;
        s_screens[i] = s;
        if (active) lv_screen_load(s.root);
        if (old_root && old_root != s.root) lv_obj_delete(old_root);
        net::logf("[ui] screen replaced: %s", id);
        return true;
    }
    return false;
}

bool set_screen_hidden(const char *id, bool hidden) {
    if (!id) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_screens[i].id, id) != 0) continue;
        s_screens[i].hidden = hidden;
        return true;
    }
    return false;
}

void show(int index) {
    if (s_count == 0) return;
    if (index < 0) index = 0;
    if (index >= (int)s_count) index = (int)s_count - 1;
    ensure_built((size_t)index);
    if (!s_screens[index].root) {
        net::logf("[ui] show: screen %s has no root (build failed?)", s_screens[index].id);
        return;
    }
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

int current_index() {
    return s_index;
}
const char *current_id() {
    return s_count == 0 ? "" : s_screens[s_index].id;
}
const char *current_title() {
    return s_count == 0 ? "" : s_screens[s_index].title;
}
size_t screen_count() {
    return s_count;
}
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

lv_obj_t *screen_root(int index) {
    if (index < 0 || index >= (int)s_count) return nullptr;
    return s_screens[index].root;
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
