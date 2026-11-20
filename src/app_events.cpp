#include "app_events.h"
#include "latency.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

#include <Arduino.h>
#include "storage.h"
#include <string.h>

#include "net.h"
#include "ui_data.h"
#include "signalk.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "knob_ui.h"
#include "midl_render.h"
#include "config_runtime.h"
#include "midl_store.h"

namespace app {

static constexpr size_t UI_QUEUE_LEN = 16;
static constexpr size_t NET_QUEUE_LEN = 8;

static QueueHandle_t s_ui_q = nullptr;
static QueueHandle_t s_net_q = nullptr;
static TaskHandle_t s_net_task = nullptr;

// Track maximum observed queue depth so /bench can surface it.
static volatile uint32_t s_ui_hi = 0;
static volatile uint32_t s_net_hi = 0;

static inline void track_depth(QueueHandle_t q, volatile uint32_t *hi) {
    if (!q) return;
    UBaseType_t n = uxQueueMessagesWaiting(q);
    if (n > *hi) *hi = n;
}

static bool is_exact_or_arg(const char *line, const char *cmd) {
    size_t n = strlen(cmd);
    return strncmp(line, cmd, n) == 0 && (line[n] == 0 || line[n] == ' ');
}

static bool is_net_command(const char *line) {
    if (!line) return false;
    return strcmp(line, "ip") == 0 || strcmp(line, "scan") == 0 || strcmp(line, "wifi-list") == 0 ||
           strcmp(line, "wifi-forget") == 0 || strcmp(line, "reboot") == 0 ||
           strcmp(line, "id") == 0 || strcmp(line, "sk-status") == 0 ||
           strcmp(line, "sk-dump") == 0 || strncmp(line, "wifi ", 5) == 0 ||
           strncmp(line, "wifi-forget ", 12) == 0 || strncmp(line, "sk ", 3) == 0 ||
           strncmp(line, "id ", 3) == 0;
}

static bool forward_to_net(Command &cmd) {
    if (post_net(cmd)) return true;
    net::logf("[app] net queue full, dropping cmd type %d", (int)cmd.type);
    if (cmd.blob) {
        heap_caps_free(cmd.blob);
        cmd.blob = nullptr;
    }
    return false;
}

// ---- net worker --------------------------------------------------------
// Drains the net queue on core 0. Anything slow (HTTP, WiFi reboot) lives
// here so the UI never blocks waiting for the network.
static void net_task(void *) {
    Command cmd;
    for (;;) {
        if (xQueueReceive(s_net_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.type) {
        case CommandType::SignalKPut: {
            int code = sk::putValue(cmd.a, cmd.b);
            net::logf("[net-worker] sk PUT %s = %s -> %d", cmd.a, cmd.b, code);
            break;
        }
        case CommandType::SaveWifi: {
            net::logf("[net-worker] saveWifi ssid='%s' (pass len %u) - rebooting", cmd.a,
                      (unsigned)strlen(cmd.b));
            net::saveWifi(String(cmd.a), String(cmd.b));  // reboots
            break;
        }
        case CommandType::Reboot: {
            net::logf("[net-worker] reboot requested");
            delay(150);
            ESP.restart();
            break;
        }
        case CommandType::RunCommand: {
            // Net-side console command (e.g. "wifi-forget", "scan").
            // Dispatch through the legacy command handlers - on this
            // task the reboot/scan is safe.
            net::dispatchCommand(String(cmd.a));
            break;
        }
        default:
            net::logf("[net-worker] unhandled cmd type %d", (int)cmd.type);
            break;
        }
        if (cmd.blob) {
            heap_caps_free(cmd.blob);
            cmd.blob = nullptr;
        }
    }
}

void setup() {
    if (s_ui_q) return;
    s_ui_q = xQueueCreate(UI_QUEUE_LEN, sizeof(Command));
    s_net_q = xQueueCreate(NET_QUEUE_LEN, sizeof(Command));
    if (!s_ui_q || !s_net_q) {
        net::logf("[app] event queue alloc FAILED");
        return;
    }
    xTaskCreatePinnedToCore(net_task, "app-net", 6144, nullptr, 2, &s_net_task, 0);
    net::logf("[app] event queues up (ui=%u, net=%u)", (unsigned)UI_QUEUE_LEN,
              (unsigned)NET_QUEUE_LEN);
}

bool post(const Command &cmd, uint32_t timeout_ms) {
    if (!s_ui_q) return false;
    // Stamp the post wall-clock so pump() can record the post->drain
    // latency for the CommandRtt benchmark channel. Producers may set
    // their own t_post_us (e.g., touch_task setting it at the point of
    // gesture detection); only fill if they didn't.
    Command stamped = cmd;
    if (stamped.t_post_us == 0) stamped.t_post_us = micros();
    bool ok = xQueueSend(s_ui_q, &stamped, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) track_depth(s_ui_q, &s_ui_hi);
    return ok;
}

bool post_net(const Command &cmd, uint32_t timeout_ms) {
    if (!s_net_q) return false;
    Command stamped = cmd;
    if (stamped.t_post_us == 0) stamped.t_post_us = micros();
    bool ok = xQueueSend(s_net_q, &stamped, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) track_depth(s_net_q, &s_net_hi);
    return ok;
}

// ---- live theme switch (no reboot) --------------------------------------
// Painters read ui::theme at build time, so flipping the palette only shows
// up on screens whose LVGL trees are rebuilt. The pump's SetTheme case flips
// the palette and then re-renders the active MIDL record (and rebuilds the
// system screens lazily), staying on the current screen. UI/LVGL task only.
static void retheme_screens() {
    midl::store_ui::rebuild();
    // lv_layer_top() overlays (MOB, alarm banner) are not part of any screen,
    // so the rebuild above never reaches them: repaint them in place (#67).
    ui::overlay_restyle();
    net::logf("[app] retheme: screens rebuilt (theme=%s, on '%s')", ui::theme_id(),
              ui::current_id());
}

// ---- UI pump (LVGL task) ----------------------------------------------
void pump() {
    if (!s_ui_q) return;
    Command cmd;
    // Drain whatever is currently queued; don't loop forever in case the
    // queue is being spammed (we'd starve LVGL).
    int max_per_tick = 8;
    while (max_per_tick-- > 0 && xQueueReceive(s_ui_q, &cmd, 0) == pdTRUE) {
        // Record post->drain latency for the CommandRtt channel.
        if (cmd.t_post_us) {
            uint32_t now = micros();
            // Defensive against wrap (~71 min).
            uint32_t dt = now - cmd.t_post_us;
            if ((int32_t)dt >= 0) {
                latency::record(latency::Channel::CommandRtt, dt);
            }
        }
        switch (cmd.type) {
        case CommandType::ShowScreen: {
            const char *id = cmd.a;
            if (!id || !*id) break;
            // Change B (cause 2): a swipe on the transient MIDL fullscreen-zoom
            // screen must RETURN to the launching screen, not navigate to a
            // sibling. The zoom is a hidden registered screen, so the swipe
            // detector's next()/prev() would skip past it and up=settings /
            // down=home would navigate away. Intercept the swipe-nav verbs
            // (next/prev/settings/home) here: if the zoom view is current,
            // dismiss_zoom() returns to the captured return id and we stop. A
            // direct id nav is left alone. Runs on the UI task (this pump), so
            // dismiss_zoom()'s ui::show is safe. Tap-to-return is unaffected
            // (it fires zoom_back_cb on LV_EVENT_CLICKED, not a ShowScreen).
            bool going_home = (strcmp(id, "home") == 0);
            bool swipe_nav = (strcmp(id, "next") == 0 || strcmp(id, "prev") == 0 ||
                              strcmp(id, "settings") == 0 || going_home);
            if (swipe_nav && midl::render::dismiss_zoom()) break;
            // Track the screen the user was on before opening settings so
            // a "close settings" swipe-down (which posts "home") returns to
            // that screen instead of always going home. Kept by id: a record
            // switch while settings is open re-indexes the registry.
            static char s_prev_before_settings[32] = "";
            const char *cur = ui::current_id();
            bool on_settings = (cur && strcmp(cur, "settings") == 0);
            bool going_settings = (strcmp(id, "settings") == 0);
            if (going_settings && !on_settings) {
                strncpy(s_prev_before_settings, cur ? cur : "", sizeof(s_prev_before_settings) - 1);
            }
            if (going_home && on_settings && s_prev_before_settings[0]) {
                char back[32];
                memcpy(back, s_prev_before_settings, sizeof(back));
                s_prev_before_settings[0] = 0;
                if (!midl::store_ui::show(back)) midl::store_ui::home();
                break;
            }
            if (strcmp(id, "next") == 0) {
                midl::store_ui::next();
            } else if (strcmp(id, "prev") == 0) {
                midl::store_ui::prev();
            } else if (going_home) {
                midl::store_ui::home();
            } else if (isdigit((unsigned char)id[0])) {
                ui::show(atoi(id));
            } else {
                midl::store_ui::show(id);  // loads the owning record when needed
            }
            break;
        }
        case CommandType::MidlActivate: {
            midl::store_ui::activate(cmd.a, cmd.i == 1 ? nullptr : cmd.b);
            break;
        }
        case CommandType::SetTheme: {
            String t(cmd.a);
            t.trim();
            if (t == "auto") {
                // Legacy manager token (#81): the manager now sends explicit
                // theme names and no longer needs "auto". Resolve it to the
                // night palette it has always booted into (main.cpp's boot
                // load falls back to night for "auto"/unknown) instead of
                // silently persisting the token and doing nothing live.
                t = "night";
            }
            if (strcmp(t.c_str(), ui::theme_id()) == 0) {
                // Palette already live, but cfg_ui and/or the legacy
                // "ui/theme" NVS string may still hold an older value
                // (power cut inside the debounce window, #76; or a stale
                // "auto" token from before this fix, #81): heal both on a
                // re-select. No rebuild needed.
                config::Theme want = config::parse_theme(t.c_str(), config::Theme::Night);
                if (config::ui().theme != want) {
                    config::Mutation m;
                    m.kind = config::MutationKind::SetTheme;
                    m.source = config::Source::External;
                    m.theme = want;
                    config::mutate(m);
                    net::logf("[ui] theme already %s (config re-synced)", t.c_str());
                } else {
                    net::logf("[ui] theme already %s", t.c_str());
                }
                bool stale_legacy_key;
                {
                    storage::Namespace p("ui", true);
                    stale_legacy_key = (p.get_string("theme", "") != t.c_str());
                }
                if (stale_legacy_key) {
                    storage::Namespace p("ui", false);
                    p.put_string("theme", t.c_str());
                    net::logf("[ui] legacy ui/theme NVS key resynced to %s", t.c_str());
                }
                break;
            }
            if (ui::use_theme(t.c_str())) {
                // Persist where boot reads it (legacy "ui" namespace string)
                // AND through the config domain model so /api/config stays
                // in sync (config_runtime debounces the NVS write).
                {
                    storage::Namespace p("ui", false);
                    p.put_string("theme", t.c_str());
                }
                config::Mutation m;
                m.kind = config::MutationKind::SetTheme;
                m.source = config::Source::External;
                m.theme = config::parse_theme(t.c_str(), config::Theme::Night);
                config::mutate(m);
                // Live switch: rebuild the built screens so painters re-read
                // ui::theme. We are on the LVGL task (pump), so this is safe.
                retheme_screens();
                net::logf("[ui] theme -> %s (live)", t.c_str());
            } else {
                net::logf("[ui] unknown theme '%s' "
                          "(day|night|high-contrast|red-night|classic|sun)",
                          t.c_str());
            }
            break;
        }
        case CommandType::SetBrightness: {
            // Route through ui::set_brightness so the config_runtime
            // cache + the LEDC pin + the persisted NVS all stay in
            // sync. Reading via ui::brightness() will then reflect
            // the new value immediately (used by /api/state.display
            // and manager F4 brightness.set tests).
            ui::set_brightness(cmd.i);
            break;
        }
        case CommandType::RunCommand: {
            if (is_net_command(cmd.a)) {
                if (forward_to_net(cmd)) return;  // net worker owns any blob
                break;
            }
            net::dispatchCommand(String(cmd.a));
            break;
        }
        case CommandType::ShowOverlay: {
            // Spec 17 §8 overlay.show. cmd.a carries the message (up
            // to 255 chars - the alarm banner truncates if it doesn't
            // fit visually).
            ui::overlay_show(cmd.a);
            break;
        }
        case CommandType::ClearOverlay: {
            ui::overlay_clear();
            break;
        }
        case CommandType::Knob:
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
            // Knob events are dispatched on the UI task: step the menu state
            // machine and perform the resulting Action (PUT / view switch /
            // overlay toggle). All LVGL mutation stays here on the UI task.
            knob_ui::apply_event(cmd.i, cmd.b[0] == '1');
#endif
            break;
        case CommandType::Select: {
            // "OK/Enter" verb (BLE HID Play/Pause, Menu-Pick, keyboard
            // Enter). Priority 1: a MIDL fullscreen-zoom overlay eats the
            // press as a dismiss, same as the swipe-nav intercept above.
            if (midl::render::dismiss_zoom()) break;
            // Otherwise: a simple toggle to/from home (the index
            // defaultScreen), remembering where we came from (by id: a record
            // switch re-indexes the registry), then hop back on the next Select.
            static char s_prev_before_select[32] = "";
            const char *cur = ui::current_id();
            char home_id[midl::records::SCREEN_ID_LEN], owner[midl::records::ID_LEN];
            midl::store::home(home_id, sizeof(home_id), owner, sizeof(owner));
            bool on_home = (cur && strcmp(cur, home_id) == 0);
            if (!on_home) {
                strncpy(s_prev_before_select, cur ? cur : "", sizeof(s_prev_before_select) - 1);
                midl::store_ui::home();
            } else if (s_prev_before_select[0]) {
                char back[32];
                memcpy(back, s_prev_before_select, sizeof(back));
                s_prev_before_select[0] = 0;
                midl::store_ui::show(back);
            }
            break;
        }
        case CommandType::SignalKPut:
        case CommandType::SaveWifi:
        case CommandType::Reboot:
            // These are net-side jobs; forward.
            if (forward_to_net(cmd)) return;  // don't free blob below - net owns it now
            break;
        default:
            break;
        }
        if (cmd.blob) {
            heap_caps_free(cmd.blob);
            cmd.blob = nullptr;
        }
    }
}

size_t ui_queue_depth() {
    return s_ui_q ? uxQueueMessagesWaiting(s_ui_q) : 0;
}
size_t net_queue_depth() {
    return s_net_q ? uxQueueMessagesWaiting(s_net_q) : 0;
}
uint32_t ui_high_water() {
    return s_ui_hi;
}
uint32_t net_high_water() {
    return s_net_hi;
}

}  // namespace app
