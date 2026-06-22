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
#include "layout_loader.h"
#include "layout_renderer.h"
#include "manager_config.h"
#include "manager_screens.h"
#include "knob_ui.h"
#include "midl_render.h"
#include "psram_json.h"

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
           strncmp(line, "id ", 3) == 0 || is_exact_or_arg(line, "layout-fetch");
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
            // Track the screen the user was on before opening settings so
            // a "close settings" swipe-down (which posts "dashboard")
            // returns to that screen instead of always Dashboard.
            static int s_prev_before_settings = -1;
            const char *cur = ui::current_id();
            bool on_settings = (cur && strcmp(cur, "settings") == 0);
            bool going_settings = (strcmp(id, "settings") == 0);
            bool going_dashboard = (strcmp(id, "dashboard") == 0);
            if (going_settings && !on_settings) {
                s_prev_before_settings = ui::current_index();
            }
            if (going_dashboard && on_settings && s_prev_before_settings >= 0) {
                ui::show(s_prev_before_settings);
                s_prev_before_settings = -1;
            } else if (strcmp(id, "next") == 0) {
                ui::next();
            } else if (strcmp(id, "prev") == 0) {
                ui::prev();
            } else if (isdigit((unsigned char)id[0])) {
                ui::show(atoi(id));
            } else {
                ui::show_by_id(id);
            }
            break;
        }
        case CommandType::ApplyLayout: {
            if (cmd.blob && cmd.blob_len) {
                bool ok = layout::apply_json((const char *)cmd.blob, cmd.blob_len);
                net::logf("[app] apply_layout %u bytes -> %s", (unsigned)cmd.blob_len,
                          ok ? "ok" : "fail");
                if (ok) {
                    // Pump runs on the LVGL task, so building LVGL widgets
                    // from the new layout::Config is safe here. Editor-shape
                    // screens swap their roots; legacy-shape screens keep
                    // their hardcoded MetricBinding tables.
                    size_t replaced = ui::layout_render::apply();
                    if (replaced)
                        net::logf("[app] layout-render: %u screens replaced", (unsigned)replaced);
                }
            }
            break;
        }
        case CommandType::SetTheme: {
            String t(cmd.a);
            t.trim();
            if (t == "day") {
                ui::use_day();
            } else if (t == "night") {
                ui::use_night();
            }
            storage::Namespace p("ui", false);
            p.put_string("theme", t.c_str());
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
        case CommandType::ApplyManagedScreens: {
            // LVGL must only be touched on the UI task. The manager
            // worker heap-allocates a RenderPlan and posts here; we
            // hand it to manager_screens::apply() and free below.
            if (cmd.blob) {
                const auto *plan_p = static_cast<const manager_config::RenderPlan *>(cmd.blob);
#ifndef YEYBOATS_MIDL_ONLY
                manager_screens::apply(*plan_p);
#else
                (void)plan_p;  // MIDL-only boot: ignore manager screens (blob freed below)
#endif
            }
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
        case CommandType::ConfigApplyMidl: {
            // Blob is a MIDL JSON document (UTF-8, not NUL-terminated).
            // cmd.a carries the optional screen_id; default "midl".
            // Runs on the LVGL task — LVGL mutations are safe here.
            // The blob is freed by the post-switch heap_caps_free below.
            if (cmd.blob && cmd.blob_len) {
                const char *sid = (cmd.a[0] != '\0') ? cmd.a : "midl";
                JsonDocument doc(&yeyboats::psram_json);
                DeserializationError err =
                    deserializeJson(doc, (const char *)cmd.blob, cmd.blob_len);
                if (err) {
                    net::logf("[app] ConfigApplyMidl: JSON parse error: %s", err.c_str());
                } else {
                    bool ok = midl::render::apply_doc(doc.as<JsonVariantConst>(), sid);
                    net::logf("[app] ConfigApplyMidl %u bytes screen='%s' -> %s",
                              (unsigned)cmd.blob_len, sid, ok ? "ok" : "fail");
                }
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
