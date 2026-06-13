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

// Minimal knob_remote registry: only the local "This knob" entry with the four
// dedicated views, matching src/knob_remote.cpp's add_local(). The render
// harness never drives the menu, so the remote/manager paths are no-ops.
namespace knob_remote {

namespace {
DisplayEntry s_local;
bool s_inited = false;

void ensure_local() {
    if (s_inited) return;
    memset(&s_local, 0, sizeof(s_local));
    strncpy(s_local.name, "This knob", sizeof(s_local.name) - 1);
    s_local.is_local = true;
    const char *vids[4] = {"ap_hud", "knob_compass", "knob_wind", "knob_big"};
    const char *vtitles[4] = {"Autopilot", "Compass", "Wind", "Big number"};
    for (int i = 0; i < 4; ++i) {
        strncpy(s_local.view_id[i], vids[i], sizeof(s_local.view_id[i]) - 1);
        strncpy(s_local.view_title[i], vtitles[i], sizeof(s_local.view_title[i]) - 1);
    }
    s_local.view_count = 4;
    s_local.current_view = 0;
    s_inited = true;
}
}  // namespace

void setup() {
    ensure_local();
}
int display_count() {
    ensure_local();
    return 1;
}
bool copy_entry(int idx, DisplayEntry &out) {
    ensure_local();
    if (idx != 0) return false;
    out = s_local;
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
