#include "knob_remote.h"

#include <string.h>

#include "ui_screens.h"

namespace knob_remote {

namespace {
DisplayEntry s_displays[kMaxDisplays];
int s_count = 0;

void add_local() {
    DisplayEntry &e = s_displays[0];
    memset(&e, 0, sizeof(e));
    strncpy(e.id, "", sizeof(e.id) - 1);
    strncpy(e.name, "This knob", sizeof(e.name) - 1);
    e.is_local = true;
    const char *vids[4] = {"ap_hud", "knob_compass", "knob_wind", "knob_big"};
    const char *vtitles[4] = {"Autopilot", "Compass", "Wind", "Big number"};
    for (int i = 0; i < 4; ++i) {
        strncpy(e.view_id[i], vids[i], sizeof(e.view_id[i]) - 1);
        strncpy(e.view_title[i], vtitles[i], sizeof(e.view_title[i]) - 1);
    }
    e.view_count = 4;
    e.current_view = 0;
    s_count = 1;
}
}  // namespace

void setup() {
    add_local();
}

int display_count() {
    return s_count;
}

const DisplayEntry *display_at(int idx) {
    if (idx < 0 || idx >= s_count) return nullptr;
    return &s_displays[idx];
}

bool switch_view(int dev_idx, int view_idx) {
    const DisplayEntry *e = display_at(dev_idx);
    if (!e || view_idx < 0 || view_idx >= e->view_count) return false;
    if (e->is_local) {
        s_displays[dev_idx].current_view = view_idx;
        return ui::show_by_id(e->view_id[view_idx]);
    }
    // Remote: filled in Phase F (manager screen.set).
    return false;
}

}  // namespace knob_remote
