#pragma once

#include <stddef.h>

// Device + view registry the knob menu navigates. Entry 0 is always the
// knob itself (local dedicated views); entries 1..N are remote displays
// discovered via the manager (Phase F). Switching resolves an index pair
// to either a local screen change or a manager screen.set command.

namespace knob_remote {

constexpr size_t kMaxDisplays = 12;
constexpr size_t kMaxViews = 12;

struct DisplayEntry {
    char id[40];    // device id ("" for the local knob -> use local)
    char name[40];  // human label
    bool is_local;
    int view_count;
    char view_id[kMaxViews][24];
    char view_title[kMaxViews][24];
    int current_view;  // index of the display's active view (-1 unknown)
};

void setup();  // seeds entry 0 (the knob's own dedicated views)

int display_count();

// Copy the entry at `idx` into `out` under the registry lock. Returns false
// (and leaves `out` untouched) if `idx` is out of range. The UI task uses this
// to read a stable snapshot while the manager worker may be rewriting entries.
bool copy_entry(int idx, DisplayEntry &out);

// Resolve a menu SwitchView action. Local -> ui::show_by_id on the knob;
// remote -> queue a manager screen.set command. Returns true if dispatched.
bool switch_view(int dev_idx, int view_idx);

// --- Manager worker -> registry ingest (Phase F) ---------------------------
// These run on the manager worker task; they take the registry lock. The UI
// task only reads via display_count() / copy_entry(). Never call the fetchers
// from the LVGL hot path.

// Refresh remote entries (1..N) from the manager's /devices/summary. Cheap GET
// run on a slow cadence (piggybacked on the manager command poll). Returns the
// number of remote displays ingested, or -1 if the manager is unreachable.
int refresh_from_manager();

// Lazily fetch the view list for a remote display (manager /devices/:id/views)
// and fill its entry. Called on drill-in (SelectView). Returns true on success.
bool fetch_views_for(int dev_idx);

// UI task -> worker: request that the manager worker fetch the view list for
// `dev_idx` on its next tick. Thread-safe; sets a small pending flag the worker
// drains. Cheap and non-blocking, safe to call from the LVGL task.
void request_views_fetch(int dev_idx);

// Worker tick: if a views fetch was requested, perform it (blocking HTTP) and
// clear the request. Called from the manager worker loop. Returns true if a
// fetch was performed.
bool drain_pending_views_fetch();

// Whether a manager-backed refresh is worth attempting (knob board, provisioned
// manager client). Lets the worker skip the GET entirely otherwise.
bool manager_available();

// --- Ingest API used by the manager client (manager.cpp) -------------------
// The manager worker owns the HTTP/JSON; these setters let it write parsed
// results into the registry under the same lock the readers use. Entry 0
// (the local knob) is never touched.

// Replace remote entries 1..N. begin_ingest() takes the lock and resets the
// remote count to zero; ingest_display() appends one (capped at kMaxDisplays);
// commit_ingest() drops the lock. Existing view lists for ids that survive the
// refresh are preserved (so a slow-cadence summary refresh doesn't wipe the
// views fetched on drill-in).
void begin_ingest();
void ingest_display(const char *id, const char *name, const char *current_screen);
void commit_ingest();

// Copy the device id of `dev_idx` into `out` (empty for the local entry / out
// of range). Lets the manager build the /devices/:id/views and command URLs.
void copy_device_id(int dev_idx, char *out, size_t out_cap);

// Set the view list for `dev_idx` (from /devices/:id/views). `current` is the
// device's active screen id (may be empty/null). Takes the lock.
void set_views(int dev_idx, const char *const *view_ids, const char *const *view_titles, int count,
               const char *current);

}  // namespace knob_remote
