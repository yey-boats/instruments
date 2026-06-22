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
#include "config_runtime.h"
#include "net.h"
#include "signalk.h"
#include "storage.h"
#include "ui_data.h"
#include "ui_screens.h"

namespace boat {
void current_view(View &out) {
    out = View{};
    out.lat = 41.38306;       // ~41 22.984 N (Barcelona)
    out.lon = 2.17222;        // ~002 10.333 E
    out.sog = 3.19;           // ~6.2 kn
    out.stw = 2.93;           // ~5.7 kn
    out.headingTrue = 0.977;  // ~56 deg
    out.cogTrue = 0.925;      // ~53 deg
    out.awa = 0.733;          // ~42 deg (stbd)
    out.aws = 6.38;           // ~12.4 kn
    out.twa = 0.80;
    out.tws = 7.46;
    out.beatAngle = 0.7330;  // ~42 deg optimal upwind TWA (polar)
    out.gybeAngle = 2.7053;  // ~155 deg optimal downwind TWA (polar)
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
}  // namespace boat

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
// Slice 3: template screen build()s register a path collector; the sim harness
// renders screens directly and has no screen registry, so this is a no-op.
bool set_screen_collect_paths(const char *, CollectPathsFn) {
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
uint8_t Namespace::get_u8(const char *, uint8_t d) {
    return d;
}
int8_t Namespace::get_i8(const char *, int8_t d) {
    return d;
}
uint16_t Namespace::get_u16(const char *, uint16_t d) {
    return d;
}
uint32_t Namespace::get_u32(const char *, uint32_t d) {
    return d;
}
bool Namespace::get_bool(const char *, bool d) {
    return d;
}
float Namespace::get_float(const char *, float d) {
    return d;
}
double Namespace::get_double(const char *, double d) {
    return d;
}
bool Namespace::put_string(const char *, const char *) {
    return true;
}
bool Namespace::put_u8(const char *, uint8_t) {
    return true;
}
bool Namespace::put_i8(const char *, int8_t) {
    return true;
}
bool Namespace::put_u16(const char *, uint16_t) {
    return true;
}
bool Namespace::put_u32(const char *, uint32_t) {
    return true;
}
bool Namespace::put_bool(const char *, bool) {
    return true;
}
bool Namespace::put_float(const char *, float) {
    return true;
}
bool Namespace::put_double(const char *, double) {
    return true;
}
bool Namespace::remove(const char *) {
    return true;
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
    // Two-line DDM, matching the real ui_data::format_position layout so the
    // zoom/position views are exercised the way they render on device.
    snprintf(buf, cap,
             "41\xC2\xB0"
             "22.984'N\n002\xC2\xB0"
             "10.333'E");
}
}  // namespace ui

namespace config {
// ui_layouts.cpp reads config::format() for the unit/precision display config.
// The sim has no NVS-backed runtime store, so return the default FormatConfig
// (the real config_runtime::format() also returns a default-constructed value
// before the store is initialised). This keeps the sim host-clean.
FormatConfig format() {
    return FormatConfig{};
}
}  // namespace config
