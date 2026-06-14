// Headless harness display/UI stubs.
//
// The harness links the SAME base infrastructure as the firmware (net.cpp and
// its dependency closure: app_events, manager, manager_screens, knob_remote,
// ...). A few of those infra modules reference display / LVGL symbols that
// normally come from src/main.cpp and src/ui/*.cpp -- TUs the harness
// deliberately excludes (no LVGL / Arduino_GFX on a headless DevKitC). This
// file provides safe no-op definitions for exactly those referenced symbols so
// the headless image links. Add entries here only in response to the linker's
// "undefined reference" errors -- nothing speculative.
//
// Gated by ESPDISP_HARNESS so it is an empty TU on the display/knob firmware
// builds (where main.cpp / src/ui provide the real implementations).
#if defined(ESPDISP_HARNESS)

#include <Arduino.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// ui:: screen-manager + theme + data surface (declared in ui_screens.h,
// ui_theme.h, ui_data.h). Referenced by app_events.cpp, manager.cpp,
// manager_screens.cpp, knob_remote.cpp. All no-ops on a headless build.
// We re-declare the minimal signatures here rather than include the ui_*.h
// headers, because those headers pull in <lvgl.h> (ui_screens.h) which is not
// available in the headless link. The signatures must match exactly.
// ---------------------------------------------------------------------------
namespace ui {

// ui_screens.h
bool show_by_id(const char *) {
    return false;
}
void show(int) {
}
void next() {
}
void prev() {
}
int current_index() {
    return 0;
}
const char *current_id() {
    return "";
}
const char *current_title() {
    return "";
}
size_t screen_count() {
    return 0;
}
bool screen_info(int, const char **out_id, const char **out_title, bool *out_hidden) {
    if (out_id) *out_id = "";
    if (out_title) *out_title = "";
    if (out_hidden) *out_hidden = false;
    return false;
}
bool set_screen_hidden(const char *, bool) {
    return false;
}
void refresh_current() {
}

// ui_data.h
uint8_t brightness() {
    return 0;
}
void set_brightness(int) {
}
void overlay_show(const char *) {
}
void overlay_clear() {
}

// ui_theme.h
void use_day() {
}
void use_night() {
}

}  // namespace ui

// ---------------------------------------------------------------------------
// ui::layout_render::apply() - app_events.cpp ConfigApplyLayout path. Lives in
// the layout renderer (LVGL) on-device; no-op here.
// ---------------------------------------------------------------------------
namespace ui {
namespace layout_render {
size_t apply() {
    return 0;
}
}  // namespace layout_render
}  // namespace ui

// ---------------------------------------------------------------------------
// knob_ui:: - app_events.cpp KnobEvent path (knob_ui::apply_event). Round-panel
// UI; no-op headless.
// ---------------------------------------------------------------------------
namespace knob_ui {
void apply_event(int, bool) {
}
}  // namespace knob_ui

// ---------------------------------------------------------------------------
// manager_screens:: - manager.cpp applies validated RenderPlans by building
// LVGL screens. The real impl (src/manager_screens.cpp) is LVGL-coupled and
// excluded from the headless build; these no-ops let manager.cpp link. We
// forward-declare the RenderPlan type only (its header pulls in <lvgl.h>).
// ---------------------------------------------------------------------------
namespace manager_config {
struct RenderPlan;
}
namespace manager_screens {
bool apply(const manager_config::RenderPlan &) {
    return false;
}
uint8_t managed_count() {
    return 0;
}
void refresh() {
}
bool is_applied() {
    return false;
}
}  // namespace manager_screens

#endif  // ESPDISP_HARNESS
