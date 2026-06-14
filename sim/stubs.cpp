// Host shims for the firmware symbols the layout code references at link time.
// Only the layout render/update paths run in the harness; the rest just need
// to resolve. sk::copyData supplies a fixed demo snapshot so widgets show
// realistic values.

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app_events.h"
#include "autopilot.h"
#include "beeper.h"
#include "net.h"
#include "signalk.h"
#include "storage.h"
#include "ui_data.h"
#include "ui_screens.h"

namespace sk {
void copyData(Data &out) {
    out = Data{};
    out.sog = 3.19;           // ~6.2 kn
    out.stw = 2.93;           // ~5.7 kn
    out.headingTrue = 0.977;  // ~56 deg
    out.cogTrue = 0.925;      // ~53 deg
    out.awa = 0.733;          // ~42 deg (stbd)
    out.aws = 6.38;           // ~12.4 kn
    out.twa = 0.80;
    out.tws = 7.46;
    out.depth = 8.4;
    out.depthKeel = 7.1;
    out.waterTemp = 292.0;  // ~18.9 C
    out.battVoltage = 12.7;
    out.battSoc = 0.82;
    out.currentSetTrue = 3.71;  // ~213 deg
    out.currentDrift = 0.7;     // ~1.4 kn
    // Autopilot: engaged in compass (auto) mode holding ~64 deg, ~8 deg off the
    // current heading, with a small cross-track error so the AP HUD render shows
    // the AUTO badge, the amber target bug, and an XTE deflection.
    std::snprintf(out.apState, sizeof(out.apState), "%s", "auto");
    out.apTargetHdg = 1.117;  // ~64 deg
    out.xte = 278.0;          // m to the right of track (~0.15 nm)
}
}  // namespace sk

namespace net {
void logf(const char *, ...) {
}
void logf_at(LogLevel, const char *, ...) {
}
}  // namespace net

namespace ui {
bool show_by_id(const char *) {
    return false;
}
}  // namespace ui

namespace beeper {
bool audible_alarms_enabled() {
    return false;
}
void set_audible_alarms(bool) {
}
}  // namespace beeper

namespace app {
bool post(const Command &, uint32_t) {
    return true;
}
bool post_net(const Command &, uint32_t) {
    return true;
}
}  // namespace app

namespace autopilot {
void copy_state(State &out) {
    out = State{};
}
Result set_mode(Mode) {
    return Result::Ok;
}
Result adjust_heading_deg(int) {
    return Result::Ok;
}
Result silence_alarm() {
    return Result::Ok;
}
}  // namespace autopilot

namespace storage {
Namespace::Namespace(const char *, bool) {
}
Namespace::~Namespace() {
}
std::string Namespace::get_string(const char *, const char *d) {
    return d ? d : "";
}
}  // namespace storage

namespace ui {
uint8_t brightness() {
    return 80;
}
PosFormat pos_format() {
    return PosFormat::DDM;
}
double depth_alarm_m() {
    return 2.0;
}
double battery_alarm_v() {
    return 11.8;
}
void format_position(double, double, PosFormat, char *buf, size_t cap) {
    snprintf(buf, cap, "41 23.0N / 002 10.3E");
}
}  // namespace ui
