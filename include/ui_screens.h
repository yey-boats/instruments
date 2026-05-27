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

namespace ui {

constexpr size_t MAX_SCREENS = 16;

struct Screen {
    const char *id;       // short canonical id, e.g. "wind"
    const char *title;    // human label, e.g. "Wind"
    lv_obj_t *root;       // fullscreen object (must be a child of lv_screen_active())
    void (*refresh)();    // called from the global 5 Hz refresh when this screen is visible
    bool hidden;          // if true, skip in swipe cycle (still reachable by id)
};

// Register a fullscreen panel. Must be called after the root LVGL object
// has been created. Order of registration defines navigation order.
void register_screen(const Screen &s);

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

// Direct access to the screen's LVGL root - needed when main wires up
// per-screen gesture / event handlers.
lv_obj_t *screen_root(int index);

// Call from the global 5 Hz ui_refresh timer.
void refresh_current();

// Diagnostics
void log_state();

}  // namespace ui
