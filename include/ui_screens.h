#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <stddef.h>

// Screen manager: each "screen" is a fullscreen 480x480 LVGL screen
// (lv_obj_create(NULL) - parentless, swapped via lv_screen_load()).
// Horizontal swipes cycle through registered screens; up swipe opens
// Settings; down swipe returns to Dashboard. Inactive screens are NOT
// in the render tree, so per-frame cost is bounded by the active screen.
// Global overlays (MOB, alarm banner, breadcrumb) attach to
// lv_layer_top() so they survive screen swaps without re-parenting.

namespace sk {
class SubscriptionSet;
}

namespace ui {

constexpr size_t MAX_SCREENS = 16;

// Per-screen path collector: appends the SignalK paths this screen consumes to
// `out` (does not clear it). Template screens register one so the subscription
// manager subscribes only what the active screen shows; screens that read
// sk::data directly (fullscreen HUDs) register none and fall back to the full
// baseline. Runs on the UI/LVGL task (the screen-change path).
typedef void (*CollectPathsFn)(sk::SubscriptionSet &out);

struct Screen {
    const char *id;                     // short canonical id, e.g. "wind"
    const char *title;                  // human label, e.g. "Wind"
    lv_obj_t *root;                     // fullscreen object (NULL until lazy-built)
    void (*refresh)();                  // called from the global 5 Hz refresh when visible
    bool hidden;                        // if true, skip in swipe cycle
    lv_obj_t *(*build_fn)(lv_obj_t *);  // lazy builder; NULL = eager (root pre-supplied)
    CollectPathsFn collect_paths;       // NULL = HUD fallback (full baseline)
    uint32_t build_us;                  // cold build_fn() duration (us); 0 until first build
};

// Register a fullscreen panel. Must be called after the root LVGL object
// has been created. Order of registration defines navigation order.
void register_screen(const Screen &s);

// Register a panel whose LVGL tree is built lazily on first activation.
// Saves boot-time heap pressure: only the initial screen + ones the user
// navigates to actually allocate LVGL widgets. `build_fn` is invoked the
// first time `show()` lands on the index; the returned root is cached.
void register_screen_lazy(const char *id, const char *title, lv_obj_t *(*build_fn)(lv_obj_t *),
                          void (*refresh_fn)(), bool hidden);

// Install a callback that runs every time a screen's root is built
// (both eager registrations and lazy ones, called once per screen).
// Use this to wire post-build setup like gesture handlers without each
// screen module having to know about main.cpp internals.
typedef void (*ScreenPostBuildCb)(lv_obj_t *root, const char *id);
void set_post_build_cb(ScreenPostBuildCb cb);

// Replace an already-registered screen in place. Useful for generated screens
// whose LVGL root can change after a central config reload.
bool replace_screen(const char *id, const Screen &s);

// Attach (or clear) a path collector for an already-registered screen by id.
// Template screens call this from their build() so the subscription manager can
// derive their path-set on show. Returns false if `id` isn't registered.
bool set_screen_collect_paths(const char *id, CollectPathsFn fn);

// Install a callback fired whenever the active screen changes (via show /
// show_by_id / next / prev / replace of the active screen). The callback gets
// the new screen's id and its CollectPathsFn (NULL = HUD fallback). main.cpp
// wires this to the per-screen subscription manager. Runs on the UI task.
typedef void (*ScreenChangeCb)(const char *id, CollectPathsFn collect);
void set_screen_change_cb(ScreenChangeCb cb);

bool set_screen_hidden(const char *id, bool hidden);

// Clear the entire registered screen set, freeing every eager-built LVGL root.
//
// Used by the MIDL renderer to atomically replace its screen set on each
// apply_all (a fresh doc push / reset / second apply must REMOVE the previous
// doc's screens, not leave them stranded with aliased refresh trampolines).
//
// LVGL invariant: there must ALWAYS be an active screen. We keep a tiny
// persistent blank "parking" screen, lv_screen_load() it first, then delete
// every dynamic root — so the live root is never the one being deleted.
// Lazy screens (build_fn != NULL, root == NULL) have nothing to free.
// Resets s_count / s_index to 0. Runs ON THE UI/LVGL task only.
void reset_screens();

// Show a specific screen by index. Out-of-range indices are clamped.
void show(int index);

// Switch by canonical id. Returns true if found.
bool show_by_id(const char *id);

// Cycle. Wraps around.
void next();
void prev();

int current_index();
const char *current_id();
const char *current_title();
size_t screen_count();
bool is_hidden(int index);

// Read-only metadata access (does NOT switch screens). Returns false if
// index is out of range; otherwise *out_* are set (nullable).
bool screen_info(int index, const char **out_id, const char **out_title, bool *out_hidden);

// Cold-build duration for screen `index` in microseconds: the time its
// build_fn() took the first (and only) time it was constructed. 0 if the
// screen was never built or index is out of range. Used by the bench sweep.
uint32_t build_us(int index);

// Direct access to the screen's LVGL root - needed when main wires up
// per-screen gesture / event handlers.
lv_obj_t *screen_root(int index);

// Call from the global 5 Hz ui_refresh timer.
void refresh_current();

// Diagnostics
void log_state();

}  // namespace ui
