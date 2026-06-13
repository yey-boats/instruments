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
const DisplayEntry *display_at(int idx);  // nullptr if out of range

// Resolve a menu SwitchView action. Local -> ui::show_by_id on the knob;
// remote -> queue a manager screen.set command. Returns true if dispatched.
bool switch_view(int dev_idx, int view_idx);

}  // namespace knob_remote
