#include "knob_remote.h"

#include <string.h>

#include "ui_screens.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "manager.h"
#endif

namespace knob_remote {

namespace {
DisplayEntry s_displays[kMaxDisplays];
int s_count = 0;
// Ingest scratch: the count being rebuilt between begin_ingest/commit_ingest.
// Defined unconditionally so the ingest functions compile on non-knob boards
// (where they are effectively no-ops).
int s_ingest_count = 0;

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Guards s_displays / s_count. The manager worker rewrites entries 1..N while
// the LVGL task reads them via copy_entry(); without the lock the reader could
// observe a half-written DisplayEntry. Entry 0 (the local knob) is immutable
// after setup() so reads of it are safe regardless.
SemaphoreHandle_t s_mtx = nullptr;
// UI-task -> worker request: which display index needs its views fetched
// (-1 = none). Written by the UI task in request_views_fetch(), drained by the
// worker. A plain volatile int is sufficient (single producer/consumer, word
// aligned); the heavier registry mutation is serialized by s_mtx.
volatile int s_pending_views_idx = -1;

// UI-task -> worker request: a remote view-switch to perform via a blocking
// manager screen.set POST. switch_view() resolves the target device id + view
// id under s_mtx and stows them here; the worker drains it on its next tick so
// the HTTP never runs on the LVGL task. Guarded by s_mtx (multi-field struct,
// unlike the single-word views index).
struct PendingSwitch {
    bool pending;
    char dev_id[40];
    char view_id[24];
};
PendingSwitch s_pending_switch = {false, {0}, {0}};

void lock() {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void unlock() {
    if (s_mtx) xSemaphoreGive(s_mtx);
}
#else
void lock() {
}
void unlock() {
}
#endif

void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void add_local() {
    DisplayEntry &e = s_displays[0];
    memset(&e, 0, sizeof(e));
    copy_str(e.id, sizeof(e.id), "");
    copy_str(e.name, sizeof(e.name), "This knob");
    e.is_local = true;
    const char *vids[4] = {"ap_hud", "knob_compass", "knob_wind", "knob_big"};
    const char *vtitles[4] = {"Autopilot", "Compass", "Wind", "Big number"};
    for (int i = 0; i < 4; ++i) {
        copy_str(e.view_id[i], sizeof(e.view_id[i]), vids[i]);
        copy_str(e.view_title[i], sizeof(e.view_title[i]), vtitles[i]);
    }
    e.view_count = 4;
    e.current_view = 0;
    s_count = 1;
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Find a remote entry [1..s_count) by device id; -1 if absent. Caller holds
// the lock.
int find_by_id_locked(const char *id) {
    if (!id || !*id) return -1;
    for (int i = 1; i < s_count; ++i) {
        if (strncmp(s_displays[i].id, id, sizeof(s_displays[i].id)) == 0) return i;
    }
    return -1;
}
#endif
}  // namespace

void setup() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
#endif
    lock();
    add_local();
    unlock();
}

int display_count() {
    lock();
    int n = s_count;
    unlock();
    return n;
}

bool copy_entry(int idx, DisplayEntry &out) {
    lock();
    bool ok = (idx >= 0 && idx < s_count);
    if (ok) out = s_displays[idx];
    unlock();
    return ok;
}

bool switch_view(int dev_idx, int view_idx) {
    // Snapshot the entry under the lock, then act on the copy.
    DisplayEntry e;
    if (!copy_entry(dev_idx, e)) return false;
    if (view_idx < 0 || view_idx >= e.view_count) return false;
    if (e.is_local) {
        lock();
        if (dev_idx >= 0 && dev_idx < s_count) s_displays[dev_idx].current_view = view_idx;
        unlock();
        return ui::show_by_id(e.view_id[view_idx]);
    }
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Remote: queue the manager screen.set for the worker. The blocking POST
    // (/devices/:id/command) must NOT run here on the LVGL task, so we only
    // resolve the target id+view under the lock, stow them in the pending slot,
    // and return true (queued). drain_pending_switch() does the HTTP on the
    // manager worker; the target executes screen.set and applies via configPush.
    lock();
    s_pending_switch.pending = true;
    copy_str(s_pending_switch.dev_id, sizeof(s_pending_switch.dev_id), e.id);
    copy_str(s_pending_switch.view_id, sizeof(s_pending_switch.view_id), e.view_id[view_idx]);
    // Optimistically reflect the requested view in the registry for the menu;
    // the actual device state is reconciled by the next summary refresh.
    if (dev_idx >= 0 && dev_idx < s_count) s_displays[dev_idx].current_view = view_idx;
    unlock();
    return true;
#else
    return false;
#endif
}

// --- Ingest API (called by the manager worker via manager.cpp) -------------

void begin_ingest() {
    lock();
    s_ingest_count = 1;  // entry 0 is the local knob, preserved
}

void ingest_display(const char *id, const char *name, const char *current_screen) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (s_ingest_count >= (int)kMaxDisplays) return;
    if (!id || !*id) return;
    // Preserve a previously-fetched view list for this id across refreshes so a
    // slow summary refresh doesn't wipe views fetched on drill-in.
    DisplayEntry prev;
    bool had_prev = false;
    int old = find_by_id_locked(id);
    if (old >= 0) {
        prev = s_displays[old];
        had_prev = true;
    }
    DisplayEntry &e = s_displays[s_ingest_count];
    memset(&e, 0, sizeof(e));
    copy_str(e.id, sizeof(e.id), id);
    copy_str(e.name, sizeof(e.name), name && *name ? name : id);
    e.is_local = false;
    e.current_view = -1;
    if (had_prev) {
        e.view_count = prev.view_count;
        for (int v = 0; v < prev.view_count && v < (int)kMaxViews; ++v) {
            memcpy(e.view_id[v], prev.view_id[v], sizeof(e.view_id[v]));
            memcpy(e.view_title[v], prev.view_title[v], sizeof(e.view_title[v]));
        }
    }
    // Resolve current view from the reported screen id if known.
    if (current_screen && *current_screen) {
        for (int v = 0; v < e.view_count; ++v) {
            if (strncmp(e.view_id[v], current_screen, sizeof(e.view_id[v])) == 0) {
                e.current_view = v;
                break;
            }
        }
    }
    s_ingest_count++;
#else
    (void)id;
    (void)name;
    (void)current_screen;
#endif
}

void commit_ingest() {
    s_count = s_ingest_count;
    unlock();
}

void copy_device_id(int dev_idx, char *out, size_t out_cap) {
    if (!out || !out_cap) return;
    out[0] = '\0';
    lock();
    if (dev_idx >= 0 && dev_idx < s_count) copy_str(out, out_cap, s_displays[dev_idx].id);
    unlock();
}

void set_views(int dev_idx, const char *const *view_ids, const char *const *view_titles, int count,
               const char *current) {
    lock();
    if (dev_idx > 0 && dev_idx < s_count) {
        DisplayEntry &e = s_displays[dev_idx];
        if (count < 0) count = 0;
        if (count > (int)kMaxViews) count = (int)kMaxViews;
        e.view_count = count;
        e.current_view = -1;
        for (int v = 0; v < count; ++v) {
            copy_str(e.view_id[v], sizeof(e.view_id[v]), view_ids ? view_ids[v] : "");
            copy_str(e.view_title[v], sizeof(e.view_title[v]),
                     view_titles && view_titles[v] ? view_titles[v] : e.view_id[v]);
            if (current && *current && strncmp(e.view_id[v], current, sizeof(e.view_id[v])) == 0) {
                e.current_view = v;
            }
        }
    }
    unlock();
}

// --- Worker-driven fetch entry points --------------------------------------

bool manager_available() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    return manager::is_provisioned();
#else
    return false;
#endif
}

int refresh_from_manager() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!manager_available()) return -1;
    return manager::knob_refresh_displays();
#else
    return -1;
#endif
}

bool fetch_views_for(int dev_idx) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!manager_available()) return false;
    return manager::knob_fetch_views(dev_idx);
#else
    (void)dev_idx;
    return false;
#endif
}

void request_views_fetch(int dev_idx) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    s_pending_views_idx = dev_idx;
#else
    (void)dev_idx;
#endif
}

bool drain_pending_views_fetch() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    int idx = s_pending_views_idx;
    if (idx < 0) return false;
    s_pending_views_idx = -1;
    return fetch_views_for(idx);
#else
    return false;
#endif
}

bool drain_pending_switch() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Copy the request out under the lock and clear it, then release the lock
    // before the blocking POST so the mutex is never held across HTTP.
    char dev_id[40];
    char view_id[24];
    lock();
    bool pending = s_pending_switch.pending;
    if (pending) {
        s_pending_switch.pending = false;
        copy_str(dev_id, sizeof(dev_id), s_pending_switch.dev_id);
        copy_str(view_id, sizeof(view_id), s_pending_switch.view_id);
    }
    unlock();
    if (!pending) return false;
    return manager::knob_post_screen_set(dev_id, view_id);
#else
    return false;
#endif
}

}  // namespace knob_remote
