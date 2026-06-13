// Host shims for the Waveshare-knob round-view render harness (env:sim-knob).
// knob_ui.cpp's four dedicated view painters only need sk::copyData + theme +
// the ui_dirty/ui_data header inlines, but the same translation unit also
// carries the dispatch core (knob_ui::*) and the menu overlay, which reference
// board::geometry(), knob_remote::*, net::logf and app::post_net at link time.
// We provide just enough here to link and to render the views with realistic
// boat values. The autopilot is seeded "auto" with a target so the ap_hud view
// shows a live mode + target + delta rather than placeholders.

#include <cstdarg>
#include <cmath>
#include <cstring>

#include "app_events.h"
#include "board.h"
#include "knob_remote.h"
#include "net.h"
#include "signalk.h"

namespace sk {
void copyData(Data &out) {
    out = Data{};
    out.sog = 3.19;           // ~6.2 kn
    out.stw = 2.93;           // ~5.7 kn
    out.headingTrue = 0.977;  // ~56 deg
    out.cogTrue = 0.925;      // ~53 deg
    out.awa = 0.733;          // ~42 deg (stbd)
    out.aws = 6.38;           // ~12.4 kn
    out.twa = 0.80;           // true wind angle
    out.tws = 7.46;           // true wind speed
    out.depth = 8.4;          // m
    out.depthKeel = 7.1;      // m
    out.waterTemp = 292.0;    // ~18.9 C
    out.battVoltage = 12.7;   // V
    out.battSoc = 0.82;       // 82 %
    out.apTargetHdg = 1.117;  // ~64 deg target (8 deg right of heading)
    strncpy(out.apState, "auto", sizeof(out.apState) - 1);
}
}  // namespace sk

namespace board {
Geometry geometry() {
    // Mirror src/boards/board_waveshare_knob.cpp: round 360x360 with the
    // inscribed-square usable inset, so the harness bounds check matches the
    // firmware's content area.
    Geometry g{};
    g.width_px = LCD_W;
    g.height_px = LCD_H;
    g.rotation = 0;
    g.square = true;
    g.shape = DisplayShape::Round;
    g.layout_class = LayoutClass::SquareCompact;
    g.density_class = DensityClass::Hdpi;
    const uint16_t inset = 53;
    g.usable_x = inset;
    g.usable_y = inset;
    g.usable_width = g.width_px - inset * 2;
    g.usable_height = g.height_px - inset * 2;
    return g;
}
}  // namespace board

namespace net {
void logf(const char *, ...) {
}
void logf_at(LogLevel, const char *, ...) {
}
}  // namespace net

namespace app {
bool post(const Command &, uint32_t) {
    return true;
}
bool post_net(const Command &, uint32_t) {
    return true;
}
}  // namespace app

// knob_remote registry stub. By default it holds only the local "This knob"
// entry with the four dedicated views, matching src/knob_remote.cpp's
// add_local(). The menu-overlay render harness (sim_knob.cpp) can SEED extra
// remote displays via the test-only sim_knob_remote_seed* helpers below so the
// real overlay code reads a real (stubbed) registry of N>kMaxRows entries and
// the SelectDisplay/SelectView windowing can be screenshotted. The seeded
// entries flow through the exact display_count()/copy_entry() API the overlay
// calls — nothing about the rendered list is faked.
namespace knob_remote {

namespace {
DisplayEntry s_entries[kMaxDisplays];
int s_count = 0;
bool s_inited = false;

void add_local() {
    DisplayEntry &e = s_entries[0];
    memset(&e, 0, sizeof(e));
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
}

void ensure_local() {
    if (s_inited) return;
    memset(s_entries, 0, sizeof(s_entries));
    add_local();
    s_count = 1;
    s_inited = true;
}
}  // namespace

void setup() {
    ensure_local();
}
int display_count() {
    ensure_local();
    return s_count;
}
bool copy_entry(int idx, DisplayEntry &out) {
    ensure_local();
    if (idx < 0 || idx >= s_count) return false;
    out = s_entries[idx];
    return true;
}
bool switch_view(int, int) {
    return true;
}
int refresh_from_manager() {
    return 0;
}
bool fetch_views_for(int) {
    return false;
}
void request_views_fetch(int) {
}
bool drain_pending_views_fetch() {
    return false;
}
bool drain_pending_switch() {
    return false;
}
bool manager_available() {
    return false;
}
void begin_ingest() {
}
void ingest_display(const char *, const char *, const char *) {
}
void commit_ingest() {
}
void copy_device_id(int, char *out, size_t out_cap) {
    if (out && out_cap) out[0] = 0;
}
void set_views(int, const char *const *, const char *const *, int, const char *) {
}
}  // namespace knob_remote

// --- Test-only seeding hooks used by sim_knob.cpp ---------------------------
// These let the render harness build a registry of N displays (the local knob
// plus remote displays) and give a chosen display a view list with a current
// marker, so the real overlay renders a faithful SelectDisplay/SelectView list.
// They mutate the same s_entries/s_count the overlay reads through copy_entry().
namespace knob_remote {
// Reset to just the local entry, then append `n_remote` synthetic remote
// displays named "Cockpit N" with a 3-view list (current = view 1). Returns the
// resulting display_count(). Capped at kMaxDisplays.
int sim_seed_displays(int n_remote) {
    ensure_local();
    s_count = 1;  // keep entry 0 (local), drop any prior remotes
    static const char *kRemoteViews[3] = {"nav", "engine", "tanks"};
    static const char *kRemoteTitles[3] = {"Navigation", "Engine", "Tanks"};
    for (int i = 0; i < n_remote && s_count < (int)kMaxDisplays; ++i) {
        DisplayEntry &e = s_entries[s_count];
        memset(&e, 0, sizeof(e));
        snprintf(e.id, sizeof(e.id), "dev-%d", s_count);
        snprintf(e.name, sizeof(e.name), "Cockpit %d", s_count);
        e.is_local = false;
        for (int v = 0; v < 3; ++v) {
            strncpy(e.view_id[v], kRemoteViews[v], sizeof(e.view_id[v]) - 1);
            strncpy(e.view_title[v], kRemoteTitles[v], sizeof(e.view_title[v]) - 1);
        }
        e.view_count = 3;
        e.current_view = 1;  // "Engine" marked current
        s_count++;
    }
    return s_count;
}
}  // namespace knob_remote
