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
#include "midl_demo_doc.h"
#include "psram_json.h"
#include "screens.h"
#include "config_runtime.h"
#include "build_config.h"

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
    // Boot palette hand-off: main's early theme load (which runs before these
    // queues exist) only knows day/night. If the persisted theme is one of
    // the extra palettes, route it through the normal SetTheme pump path so
    // the palette flips and the (still dashboard-only) screen set rebuilds
    // on the first pump pass. No-op for day/night (already applied by boot).
    {
        storage::Namespace p("ui", true);
        std::string pref = p.get_string("theme", "night");
        if (pref != "day" && pref != "night" && ui::theme_known(pref.c_str())) {
            Command c;
            c.type = CommandType::SetTheme;
            strncpy(c.a, pref.c_str(), sizeof(c.a) - 1);
            post(c);
        }
    }
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
// the palette and then rebuilds the current screen set THE SAME WAY it was
// built, using existing public entry points only (everything below runs on
// the UI/LVGL task):
//   - MIDL set: re-apply the last successfully applied MIDL doc (we keep a
//     PSRAM copy; midl::render::apply_doc resets + rebuilds atomically).
//   - Classic set: ui::reset_screens() + re-register the boot set lazily
//     (mirror of main.cpp's registration table) — only the screen the user
//     is on rebuilds immediately; others rebuild on first visit.
//   - Editor layout screens: re-run ui::layout_render::apply() if an
//     ApplyLayout happened this session (same entry point layout-fetch uses).

// PSRAM copy of the last MIDL doc the pump successfully applied, so a theme
// flip can re-apply it. Covers /api/midl/config, /api/midl/reset and the
// `midl-render` console command (all funnel through ConfigApplyMidl).
static char *s_midl_doc = nullptr;
static size_t s_midl_doc_len = 0;
static char s_midl_sid[32] = "";

// True once ui::layout_render::apply() replaced screens this session (the
// ApplyLayout pump case). A boot-time layout::load_default() alone does NOT
// materialize editor screens, so the flag — not layout::loaded() — decides
// whether a theme rebuild re-applies the layout.
static bool s_layout_applied = false;

static void remember_midl_doc(const char *bytes, size_t len, const char *sid) {
    char *copy = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        net::logf("[app] midl doc keep-alive alloc failed (%u bytes)", (unsigned)len);
        return;
    }
    memcpy(copy, bytes, len);
    copy[len] = 0;
    if (s_midl_doc) heap_caps_free(s_midl_doc);
    s_midl_doc = copy;
    s_midl_doc_len = len;
    strncpy(s_midl_sid, sid ? sid : "", sizeof(s_midl_sid) - 1);
    s_midl_sid[sizeof(s_midl_sid) - 1] = 0;
    // apply_all reset the whole screen set; any earlier layout apply is gone.
    s_layout_applied = false;
}

#ifndef YEYBOATS_MIDL_ONLY
// Mirror of main.cpp setup()'s screen registration (ids/titles/builders/
// hidden flags and order must stay in lockstep). All lazy: after a theme
// flip only the screen being shown rebuilds now, the rest on first visit.
static void register_boot_screens() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    ui::register_screen_lazy("ap_hud", "Autopilot", ui::ap_hud::build, ui::ap_hud::refresh, false);
    ui::register_screen_lazy("knob_compass", "Compass", ui::knob_compass::build,
                             ui::knob_compass::refresh, false);
    ui::register_screen_lazy("knob_wind", "Wind", ui::knob_wind::build, ui::knob_wind::refresh,
                             false);
    ui::register_screen_lazy("knob_big", "Big", ui::knob_big::build, ui::knob_big::refresh, false);
#else
    ui::register_screen_lazy("dashboard", "Dashboard", ui::dashboard::build, ui::dashboard::refresh,
                             false);
    ui::register_screen_lazy("zoom", "Zoom", ui::zoom::build, ui::zoom::refresh, true);
    ui::register_screen_lazy("wind", "Wind", ui::wind::build, ui::wind::refresh, false);
    ui::register_screen_lazy("wind_classic", "Wind (classic)", ui::wind_classic::build,
                             ui::wind_classic::refresh, false);
    ui::register_screen_lazy("wind_steer", "Wind Steer", ui::wind_steer::build,
                             ui::wind_steer::refresh, false);
    ui::register_screen_lazy("nav", "Nav", ui::nav::build, ui::nav::refresh, false);
    ui::register_screen_lazy("depth", "Depth", ui::depth::build, ui::depth::refresh, false);
    ui::register_screen_lazy("steering", "Steering", ui::steering::build, ui::steering::refresh,
                             false);
    ui::register_screen_lazy("route", "Route", ui::route::build, ui::route::refresh, false);
    ui::register_screen_lazy("autopilot", "Autopilot", ui::autopilot::build, ui::autopilot::refresh,
                             false);
    ui::register_screen_lazy("trip", "Trip", ui::trip::build, ui::trip::refresh, false);
    ui::register_screen_lazy("status", "System", ui::status_panel::build, ui::status_panel::refresh,
                             false);
    ui::register_screen_lazy("wifi", "WiFi Setup", ui::wifi_setup::build, ui::wifi_setup::refresh,
                             true);
    ui::register_screen_lazy("settings", "Settings", ui::settings::build, ui::settings::refresh,
                             true);
#if YEYBOATS_ENABLE_TOUCH_CAL_UI
    ui::register_screen_lazy("touch_cal", "Touch Cal", ui::touch_cal_screen::build,
                             ui::touch_cal_screen::refresh, true);
#endif
    ui::register_screen_lazy("touch_grid", "Touch Grid", ui::touch_grid_screen::build,
                             ui::touch_grid_screen::refresh, true);
    ui::register_screen_lazy("demo_grid", "Demo Grid", ui::demo_grid::build, ui::demo_grid::refresh,
                             true);
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8
}
#endif  // !YEYBOATS_MIDL_ONLY

// Rebuild the built screens after a palette flip. UI/LVGL task only.
static void retheme_screens() {
    // The current id points into the registry we are about to reset — copy it.
    char cur[32] = "";
    strncpy(cur, ui::current_id(), sizeof(cur) - 1);

    bool midl_set = false;
    if (s_midl_doc) {
        // MIDL set active: re-apply the kept doc (apply_doc -> apply_all
        // resets + rebuilds the whole set atomically with the new palette).
        JsonDocument doc(&yeyboats::psram_json);
        if (deserializeJson(doc, s_midl_doc, s_midl_doc_len) == DeserializationError::Ok) {
            midl_set = midl::render::apply_doc(doc.as<JsonVariantConst>(),
                                               s_midl_sid[0] ? s_midl_sid : nullptr);
        }
        if (!midl_set) net::logf("[app] retheme: midl re-apply failed");
    }
    if (!midl_set) {
#ifdef YEYBOATS_MIDL_ONLY
        // MIDL-only boot renders the baked demo doc directly from setup(),
        // bypassing the pump — rebuild from the same baked doc.
        JsonDocument doc(&yeyboats::psram_json);
        if (deserializeJson(doc, midl::demo::SQUARE_480_JSON) == DeserializationError::Ok) {
            midl_set = midl::render::apply_doc(doc.as<JsonVariantConst>(), nullptr);
        }
#else
        // Classic boot set: drop every built root and re-register lazily.
        ui::reset_screens();
        register_boot_screens();
#endif
    }
    // Editor layout screens replace matching ids in place (same entry point
    // as the ApplyLayout pump case / layout-fetch).
    if (s_layout_applied) ui::layout_render::apply();
    // Return to where the user was; the apply/registration default otherwise.
    if (!ui::show_by_id(cur) && !midl_set) ui::show(0);
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
            // down=dashboard would navigate away. Intercept the swipe-nav verbs
            // (next/prev/settings/dashboard) here: if the zoom view is current,
            // dismiss_zoom() returns to the captured return id and we stop. A
            // direct id nav is left alone. Runs on the UI task (this pump), so
            // dismiss_zoom()'s ui::show is safe. Tap-to-return is unaffected
            // (it fires zoom_back_cb on LV_EVENT_CLICKED, not a ShowScreen).
            bool swipe_nav = (strcmp(id, "next") == 0 || strcmp(id, "prev") == 0 ||
                              strcmp(id, "settings") == 0 || strcmp(id, "dashboard") == 0);
            if (swipe_nav && midl::render::dismiss_zoom()) break;
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
                    if (replaced) {
                        net::logf("[app] layout-render: %u screens replaced", (unsigned)replaced);
                        s_layout_applied = true;  // theme rebuilds re-apply it
                    }
                }
            }
            break;
        }
        case CommandType::SetTheme: {
            String t(cmd.a);
            t.trim();
            if (strcmp(t.c_str(), ui::theme_id()) == 0) {
                net::logf("[ui] theme already %s", t.c_str());
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
            } else if (t == "auto") {
                // Legacy manager token: persisted verbatim, renders as night.
                storage::Namespace p("ui", false);
                p.put_string("theme", t.c_str());
            } else {
                net::logf("[ui] unknown theme '%s' "
                          "(day|night|high-contrast|red-night|classic)",
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
                    // Keep a copy of the applied doc so a live theme switch
                    // can rebuild the MIDL screen set with the new palette.
                    if (ok) remember_midl_doc((const char *)cmd.blob, cmd.blob_len, sid);
                }
            }
            break;
        }
        case CommandType::Select: {
            // "OK/Enter" verb (BLE HID Play/Pause, Menu-Pick, keyboard
            // Enter). Priority 1: a MIDL fullscreen-zoom overlay eats the
            // press as a dismiss, same as the swipe-nav intercept above.
            if (midl::render::dismiss_zoom()) break;
            // Otherwise: a simple toggle to/from the dashboard, mirroring
            // the settings return-tracking above (remember where we came
            // from, then hop back on the next Select).
            static int s_prev_before_select = -1;
            const char *cur = ui::current_id();
            bool on_dashboard = (cur && strcmp(cur, "dashboard") == 0);
            if (!on_dashboard) {
                s_prev_before_select = ui::current_index();
                ui::show_by_id("dashboard");
            } else if (s_prev_before_select >= 0) {
                ui::show(s_prev_before_select);
                s_prev_before_select = -1;
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
