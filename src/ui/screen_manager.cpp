#include "ui_screens.h"

#include "net.h"

namespace ui {

static Screen s_screens[MAX_SCREENS];
static size_t s_count = 0;
static int s_index = 0;
static ScreenPostBuildCb s_post_build_cb = nullptr;
static ScreenChangeCb s_change_cb = nullptr;

// Notify the subscription manager that screen `i` is now active. Called only
// when the active screen actually changes (show() guards re-loads). The
// callback hands off the new screen's path-collector; the manager itself
// computes the desired set and records it for the SK task to apply. We do NOT
// build a SubscriptionSet here - it's a ~5 KB struct and this runs on the UI
// loopTask (8 KB stack); the manager owns the (static) scratch sets.
static void notify_change(size_t i) {
    if (!s_change_cb || i >= s_count) return;
    s_change_cb(s_screens[i].id, s_screens[i].collect_paths);
}

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
        // [nav-diag] First-screen auto-load: confirm LVGL actually moved the
        // active screen onto this root (and OFF any parking root left by a
        // prior reset_screens). If active != s.root here, the subsequent
        // show(N) path must not early-return on a stale guard.
        net::logf("[ui] register auto-show '%s' root=%p active=%p s_index=0", s.id ? s.id : "?",
                  (void *)s.root, (void *)lv_screen_active());
        notify_change(0);  // the boot screen drives the initial subscription set
    }
    if (s.root && s_post_build_cb) s_post_build_cb(s.root, s.id);
}

void set_screen_change_cb(ScreenChangeCb cb) {
    s_change_cb = cb;
    // Drive the initial subscription set for whatever screen is already active
    // (boot registers the first screen before main wires this callback).
    if (cb && s_count) notify_change((size_t)s_index);
}

bool set_screen_collect_paths(const char *id, CollectPathsFn fn) {
    if (!id) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_screens[i].id, id) != 0) continue;
        s_screens[i].collect_paths = fn;
        // If this is the active screen, refresh its subscription set now that
        // we know how to collect its paths (template build() registers late).
        if ((int)i == s_index) notify_change(i);
        return true;
    }
    return false;
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
    uint32_t t0 = micros();
    s_screens[i].root = s_screens[i].build_fn(nullptr);
    s_screens[i].build_us = micros() - t0;
    net::logf("[ui] lazy-built screen %s root=%p in %lu us", s_screens[i].id, s_screens[i].root,
              (unsigned long)s_screens[i].build_us);
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
        // Fire the post-build callback so swipes work on the new root
        // (the gesture handler attached to the deleted old_root is gone).
        if (s.root && s_post_build_cb) s_post_build_cb(s.root, s_screens[i].id);
        if (active) notify_change(i);  // re-diff subs if the live screen changed
        net::logf("[ui] screen replaced: %s", id);
        return true;
    }
    return false;
}

// Tiny persistent blank screen used as a "parking" root during reset_screens()
// so LVGL always has a live active screen while we delete the dynamic roots.
// Built once on first reset, never deleted. lv_obj_create(NULL) -> parentless
// fullscreen object, exactly like a registered screen root.
static lv_obj_t *s_parking_root = nullptr;

// True when the LVGL active screen is the blank parking root — i.e. we are
// mid-reset / between screen sets and no registered screen is actually shown.
// show() uses this so it can never early-return into a no-op while parked.
static bool active_is_parking() {
    return s_parking_root != nullptr && lv_screen_active() == s_parking_root;
}

void reset_screens() {
    // Park on a persistent blank screen FIRST so the root we're about to delete
    // is never the active screen (LVGL must always have one loaded).
    if (!s_parking_root) {
        s_parking_root = lv_obj_create(nullptr);
    }
    if (s_parking_root) {
        lv_screen_load(s_parking_root);
    }

    // Free every eager-built root. Lazy screens (build_fn != NULL) that were
    // never shown have root == NULL and need no free; lazy screens that WERE
    // built cached their root and must be freed too. Guard against deleting the
    // parking root (a registered screen could never alias it, but be defensive).
    size_t deleted = 0;
    for (size_t i = 0; i < s_count; ++i) {
        lv_obj_t *root = s_screens[i].root;
        if (root && root != s_parking_root) {
            lv_obj_delete(root);
            ++deleted;
        }
        s_screens[i] = Screen{};
    }

    s_count = 0;
    s_index = 0;
    // [nav-diag] Report how many roots were freed and the parking pointer LVGL
    // is now parked on, so a post-push log shows the teardown completed and the
    // active screen is the parking root before the rebuild auto-loads screen 0.
    net::logf("[ui] screens reset: deleted %u root(s), parked=%p active=%p", (unsigned)deleted,
              (void *)s_parking_root, (void *)lv_screen_active());
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
    lv_obj_t *target = s_screens[index].root;
    lv_obj_t *active = lv_screen_active();
    bool parked = active_is_parking();
    // [nav-diag] One line per show() so the on-hardware repro is readable in
    // /api/logs: requested index, target root, current active root (and whether
    // it's the parking root), and the manager's s_index. After a live-push
    // reset_screens this is the line that reveals whether show(1)/show(2)
    // actually loads or hits an early-return.
    net::logf("[ui] show(%d) id=%s target=%p active=%p parked=%d s_index=%d", index,
              s_screens[index].id ? s_screens[index].id : "?", (void *)target, (void *)active,
              parked ? 1 : 0, s_index);
    if (!target) {
        net::logf("[ui] show: screen %s has no root (build failed?)", s_screens[index].id);
        return;
    }
    // Early-return ONLY when this is a genuine no-op: the requested root is the
    // one LVGL already has loaded AND the manager index already matches. Crucially
    // this is gated on NOT being parked — after reset_screens parks on the blank
    // root, the active screen is the parking root, so we must always issue the
    // load to move off it (an `active==target` check alone could otherwise be
    // fooled by a stale pointer and strand the UI on the first/parking screen).
    if (!parked && active == target && index == s_index) {
        net::logf("[ui] show(%d): no-op (already active)", index);
        return;
    }
    s_index = index;
    lv_screen_load(s_screens[s_index].root);
    net::logf("[ui] screen -> %d (%s) loaded active=%p", s_index, s_screens[s_index].id,
              (void *)lv_screen_active());
    notify_change((size_t)s_index);  // re-diff the per-screen subscription set
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

uint32_t build_us(int index) {
    if (index < 0 || index >= (int)s_count) return 0;
    return s_screens[index].build_us;
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
