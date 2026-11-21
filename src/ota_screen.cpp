// Device half of the firmware-update screen (instruments#106). See
// include/ota_screen.h for the call contract and include/ota_progress.h for
// the pacing rules and the lit/dark switch.
//
// Handshake: the OTA task fills one request under s_mtx, gives s_req and
// waits on s_ack. The paused UI task (lvgl_task -> ui_service) takes the
// request, builds/updates the LVGL screen, lv_refr_now()s it (the flush
// callback writes the dirty rows back to the PSRAM framebuffer), waits for
// the panel to scan it out, then acks. While the OTA task waits it is not
// inside Update.write, so the redraw never overlaps a flash erase/write.
// A request the UI task has not picked up when the wait times out is
// withdrawn, so a late UI task never renders in the middle of a write.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "ota_screen.h"

#if defined(YEYBOATS_HARNESS)

// Headless harness: no panel.
namespace ota_screen {
bool begin(const char *, const char *, uint32_t) {
    return false;
}
bool active() {
    return false;
}
void progress(uint32_t, uint32_t) {
}
void status(const char *) {
}
void succeeded() {
}
bool failed(const char *) {
    return false;
}
void ui_service(uint32_t) {
}
void ui_poll() {
}
}  // namespace ota_screen

#else

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <string.h>

#include "board.h"
#include "build_version.h"
#include "config_runtime.h"
#include "net.h"
#include "ota_progress.h"

namespace ota_screen {
namespace {

enum class Req : uint8_t { None, Show, Update, Done, Failed, Restore };
enum class Slot : uint8_t { Idle, Pending, Serving };

// One request, copied by value (small; internal RAM is fine).
struct Request {
    Req req = Req::None;
    int pct = -1;  // -1 = indeterminate
    uint32_t phase = 0;
    char title[32] = {};
    char version[48] = {};
    char bytes[32] = {};
    char status[64] = {};
};

StaticSemaphore_t s_mtx_buf, s_req_buf, s_ack_buf;
SemaphoreHandle_t s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
SemaphoreHandle_t s_req = xSemaphoreCreateBinaryStatic(&s_req_buf);
SemaphoreHandle_t s_ack = xSemaphoreCreateBinaryStatic(&s_ack_buf);

Slot s_slot = Slot::Idle;
Request s_pending;

// OTA-task state (one OTA at a time).
volatile bool s_active = false;
volatile bool s_restore_owed = false;  // restore request timed out: UI task does it on resume
bool s_shown = false;
ota_progress::Pacer s_pacer;
uint32_t s_total = 0;
uint32_t s_done = 0;
uint32_t s_t0 = 0;
char s_status[64] = {};

// UI-task state.
lv_obj_t *s_scr = nullptr;
lv_obj_t *s_prev = nullptr;
bool s_top_was_hidden = false;
lv_obj_t *s_title = nullptr;
lv_obj_t *s_version = nullptr;
lv_obj_t *s_bar = nullptr;
lv_obj_t *s_bytes = nullptr;
lv_obj_t *s_statusl = nullptr;

// One panel frame is ~24 ms (480x480 @ 12 MHz PCLK); wait two so the RGB DMA
// has fetched the new rows before the next flash write can stall it.
constexpr uint32_t kSettleMs = 50;
constexpr uint32_t kShowWaitMs = 2000;
constexpr uint32_t kUpdateWaitMs = 500;
constexpr uint32_t kServingGraceMs = 3000;  // UI task already rendering: let it finish
constexpr uint32_t kFailHoldMs = 3000;      // failure message on screen before restore

// Dim colours on black: the theory under test is that a near-uniform dark
// frame hides the scan-out drift during flash writes.
constexpr uint32_t kText = 0xB8B8B8;
constexpr uint32_t kDim = 0x707070;
constexpr uint32_t kBarBg = 0x1C1C1C;
constexpr uint32_t kBarFg = 0x2F6FC0;
constexpr uint32_t kFail = 0xD08A2A;

struct Lock {
    Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mtx); }
};

void copy(char *dst, size_t n, const char *src) {
    if (!n) return;
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
}

// OTA task: hand `r` to the UI task and wait until it is on the panel.
bool post(const Request &r, uint32_t wait_ms) {
    xSemaphoreTake(s_ack, 0);  // drop a stale ack from a timed-out request
    {
        Lock l;
        s_pending = r;
        s_slot = Slot::Pending;
    }
    xSemaphoreGive(s_req);
    if (xSemaphoreTake(s_ack, pdMS_TO_TICKS(wait_ms)) == pdTRUE) return true;
    {
        Lock l;
        if (s_slot == Slot::Pending) {  // never picked up: withdraw it
            s_slot = Slot::Idle;
            return false;
        }
    }
    return xSemaphoreTake(s_ack, pdMS_TO_TICKS(kServingGraceMs)) == pdTRUE;
}

Request snapshot(Req req) {
    Request r;
    r.req = req;
    r.pct = s_total ? s_pacer.shown_pct() : -1;
    if (r.pct < -1) r.pct = 0;
    r.phase = s_pacer.phase();
    ota_progress::format_bytes(r.bytes, sizeof(r.bytes), s_done, s_total);
    copy(r.status, sizeof(r.status), s_status);
    return r;
}

// ---- UI task --------------------------------------------------------------

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(90));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    return l;
}

void build() {
    const int32_t w = lv_display_get_horizontal_resolution(nullptr);
    const int32_t h = lv_display_get_vertical_resolution(nullptr);
    const int32_t small = w < h ? w : h;
    const bool compact = small < 320;  // 240/360 round panels

    s_prev = lv_screen_active();
    lv_obj_t *top = lv_layer_top();
    s_top_was_hidden = lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);  // MOB/alarm/control overlays off

    s_scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scr, compact ? 6 : 12, 0);

    s_title = label(s_scr, compact ? &font_b612_20 : &font_b612_28, kText);
    s_version = label(s_scr, &font_b612_14, kDim);

    s_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar, (small * 62) / 100, compact ? 8 : 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(kBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(kBarFg), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_bar, 0, LV_PART_MAIN);

    s_bytes = label(s_scr, &font_b612_14, kDim);
    s_statusl = label(s_scr, &font_b612_14, kDim);

    lv_screen_load(s_scr);  // no animation: swaps the active screen now
}

void apply(const Request &r) {
    if (!s_scr) return;
    if (r.title[0]) lv_label_set_text(s_title, r.title);
    if (r.version[0]) lv_label_set_text(s_version, r.version);
    if (r.pct >= 0) {
        lv_bar_set_mode(s_bar, LV_BAR_MODE_NORMAL);
        lv_bar_set_value(s_bar, r.pct, LV_ANIM_OFF);
    } else {
        // Unknown size: a 20 % segment that steps across and back.
        lv_bar_set_mode(s_bar, LV_BAR_MODE_RANGE);
        int x = ota_progress::bounce_x(r.phase, 100, 20, 10);
        lv_bar_set_start_value(s_bar, x, LV_ANIM_OFF);
        lv_bar_set_value(s_bar, x + 20, LV_ANIM_OFF);
    }
    lv_label_set_text(s_bytes, r.bytes);
    lv_label_set_text(s_statusl, r.status);
}

void restore() {
    if (!s_scr) return;
    if (s_prev && lv_obj_is_valid(s_prev)) lv_screen_load(s_prev);
    lv_obj_delete(s_scr);
    s_scr = s_title = s_version = s_bar = s_bytes = s_statusl = nullptr;
    if (!s_top_was_hidden) lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    s_prev = nullptr;
    lv_obj_invalidate(lv_screen_active());
}

void lit() {
    board::set_backlight(::config::ui().brightness);  // user's value, never persisted
}

}  // namespace

void ui_poll() {
    if (!s_restore_owed) return;
    s_restore_owed = false;
    restore();
    lit();
    net::logf("[ota-ui] display restored on resume");
}

void ui_service(uint32_t wait_ms) {
    if (xSemaphoreTake(s_req, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return;
    Request r;
    {
        Lock l;
        if (s_slot != Slot::Pending) return;  // withdrawn
        r = s_pending;
        s_slot = Slot::Serving;
    }
    const bool dark = ota_progress::kMode == ota_progress::Mode::Dark;
    switch (r.req) {
    case Req::Show:
        if (!s_scr) build();
        apply(r);
        if (dark) {
            board::set_backlight(0);  // comparison mode: dark for the whole write
        } else {
            lit();
        }
        break;
    case Req::Update:
        apply(r);
        break;
    case Req::Done:
    case Req::Failed:
        apply(r);
        if (r.req == Req::Failed && s_title)
            lv_obj_set_style_text_color(s_title, lv_color_hex(kFail), 0);
        break;
    case Req::Restore:
        restore();
        break;
    default:
        break;
    }
    lv_refr_now(nullptr);
    // End states are shown lit in both modes (no flash write follows the
    // "Restarting" frame; the failure frame is the hand-back to the user).
    if (r.req == Req::Done || r.req == Req::Failed || r.req == Req::Restore) lit();
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    {
        Lock l;
        s_slot = Slot::Idle;
    }
    xSemaphoreGive(s_ack);
}

bool begin(const char *source, const char *version, uint32_t total) {
    if (s_active) return s_shown;
    s_active = true;
    s_pacer.reset();
    s_total = total;
    s_done = 0;
    s_t0 = millis();
    copy(s_status, sizeof(s_status), "Do not power off");
    s_pacer.due(0, total);

    Request r = snapshot(Req::Show);
    copy(r.title, sizeof(r.title), "Updating firmware");
    if (version && version[0]) {
        snprintf(r.version, sizeof(r.version), "%s -> %s", FW_VERSION, version);
    } else {
        snprintf(r.version, sizeof(r.version), "from %s (%s)", FW_VERSION,
                 source && source[0] ? source : "ota");
    }
    s_shown = post(r, kShowWaitMs);
    net::logf("[ota-ui] update screen %s (%s, %u ms)", s_shown ? "up" : "TIMEOUT",
              ota_progress::mode_name(ota_progress::kMode), (unsigned)(millis() - s_t0));
    return s_shown;
}

bool active() {
    return s_active;
}

void progress(uint32_t done, uint32_t total) {
    if (!s_active) return;
    s_done = done;
    s_total = total;
    if (!s_pacer.due(done, total)) return;
    if (ota_progress::kMode == ota_progress::Mode::Dark) return;  // nothing visible
    post(snapshot(Req::Update), kUpdateWaitMs);
}

void status(const char *text) {
    if (!s_active) return;
    copy(s_status, sizeof(s_status), text);
    if (ota_progress::kMode == ota_progress::Mode::Dark) return;
    post(snapshot(Req::Update), kUpdateWaitMs);
}

void succeeded() {
    if (!s_active) return;
    s_done = s_total;
    s_pacer.due(s_total, s_total);
    copy(s_status, sizeof(s_status), "Restarting...");
    Request r = snapshot(Req::Done);
    if (!s_total) r.pct = 100;
    copy(r.title, sizeof(r.title), "Update complete");
    post(r, kUpdateWaitMs);
    net::logf("[ota-ui] complete in %u ms, restarting", (unsigned)(millis() - s_t0));
}

bool failed(const char *detail) {
    if (!s_active) return false;
    snprintf(s_status, sizeof(s_status), "%s", detail && detail[0] ? detail : "unknown error");
    Request r = snapshot(Req::Failed);
    copy(r.title, sizeof(r.title), "Update failed");
    post(r, kUpdateWaitMs);
    vTaskDelay(pdMS_TO_TICKS(kFailHoldMs));
    Request back;
    back.req = Req::Restore;
    bool ok = post(back, kShowWaitMs);
    if (!ok) s_restore_owed = true;
    net::logf("[ota-ui] failed (%s); display %s", s_status, ok ? "restored" : "restore TIMEOUT");
    s_active = false;
    s_shown = false;
    return true;
}

}  // namespace ota_screen

#endif  // YEYBOATS_HARNESS
