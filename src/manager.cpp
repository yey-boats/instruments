#include "manager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include "app_events.h"
#include "beeper.h"
#include "board.h"
#include "device_identity.h"
#include "font_resolver.h"
#include "manager_config.h"
#include "net.h"
#include "signalk.h"

namespace manager {

namespace {

constexpr const char *NS = "mgr";
constexpr uint32_t DEFAULT_HEARTBEAT_MS = 30000;
constexpr uint32_t DEFAULT_COMMAND_POLL_MS = 10000;

String s_endpoint;
String s_token;
AuthState s_auth = AuthState::Unprovisioned;
HealthState s_health = HealthState::Idle;
uint32_t s_heartbeat_interval_ms = DEFAULT_HEARTBEAT_MS;
uint32_t s_command_poll_interval_ms = DEFAULT_COMMAND_POLL_MS;
// F3 - last applied central config. When the plugin's heartbeat
// response advertises a different desired_config_{version,hash},
// the worker fetches /devices/:id/config and applies it.
String s_applied_config_version = "v0";
String s_applied_config_hash = "";
// Parsed RenderPlan from the last successfully-validated config.
// Read by D7 diagnostic commands and (future D5) by the screen
// builder. Holds zeroed defaults until first valid config lands.
manager_config::RenderPlan s_render_plan;
bool s_render_plan_valid = false;
String s_desired_config_version = "";
String s_desired_config_hash = "";
volatile bool s_config_fetch_pending = false;
volatile uint32_t s_last_register_ms = 0;
volatile int s_last_register_code = 0;
volatile uint32_t s_last_heartbeat_ms = 0;
volatile int s_last_heartbeat_code = 0;
TaskHandle_t s_task = nullptr;
volatile bool s_force_register = false;

// F6 pull-OTA state. Single in-flight job at a time; if a second
// command arrives while busy we ack busy.
TaskHandle_t s_ota_task = nullptr;
volatile bool s_ota_in_flight = false;
String s_ota_job_id;
String s_ota_url;
String s_ota_sha256;
String s_ota_version;
size_t s_ota_size = 0;
// Set in setup() iff this boot came from a freshly-installed OTA
// partition (PENDING_VERIFY). The next successful heartbeat will POST
// /firmware/confirm to the manager so the plugin can mark the OTA job
// complete, then clear the flag.
volatile bool s_ota_confirm_pending = false;
SemaphoreHandle_t s_state_mtx = nullptr;

void lock_state() {
    if (s_state_mtx) xSemaphoreTake(s_state_mtx, portMAX_DELAY);
}
void unlock_state() {
    if (s_state_mtx) xSemaphoreGive(s_state_mtx);
}

void load_prefs() {
    Preferences p;
    p.begin(NS, true);
    s_endpoint = p.getString("endpoint", "");
    s_token = p.getString("token", "");
    s_applied_config_version = p.getString("cfg_ver", "v0");
    s_applied_config_hash = p.getString("cfg_hash", "");
    p.end();
    s_auth = s_token.length() ? AuthState::Provisioned
                              : AuthState::Unprovisioned;
}

void save_prefs() {
    Preferences p;
    p.begin(NS, false);
    p.putString("endpoint", s_endpoint);
    p.putString("token", s_token);
    p.putString("cfg_ver", s_applied_config_version);
    p.putString("cfg_hash", s_applied_config_hash);
    p.end();
}

String build_url(const char *path) {
    String url = s_endpoint;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    url += path;
    return url;
}

String build_url_from_base(const String &base, const char *path) {
    String url = base;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    url += path;
    return url;
}

String plugin_base_from_root(const String &endpoint) {
    String base = endpoint;
    if (base.endsWith("/")) base.remove(base.length() - 1);
    if (base.indexOf("/plugins/espdisp-manager") >= 0) return base;
    return base + "/plugins/espdisp-manager";
}

bool endpoint_has_path(const String &endpoint) {
    int scheme = endpoint.indexOf("://");
    int start = scheme >= 0 ? scheme + 3 : 0;
    return endpoint.indexOf('/', start) >= 0;
}

// POST /devices/register. On 200, store the returned bearer token +
// any heartbeat/command-poll intervals the plugin reports.
int do_register() {
    if (s_endpoint.length() == 0) return -1;
    if (WiFi.status() != WL_CONNECTED) return -2;
    s_health = HealthState::Registering;

    JsonDocument body;
    device_identity::to_json_doc(body);
    String payload;
    serializeJson(body, payload);

    String bases[2];
    int base_count = 0;
    // If the user provides a bare SignalK server root, try the real
    // plugin route first. If they provide a mock manager URL or an
    // explicit path, try it as-is first.
    if (!endpoint_has_path(s_endpoint)) {
        bases[base_count++] = plugin_base_from_root(s_endpoint);
        bases[base_count++] = s_endpoint;
    } else {
        bases[base_count++] = s_endpoint;
    }

    int code = -3;
    String resp;
    String successful_base;
    for (int i = 0; i < base_count; ++i) {
        HTTPClient http;
        String url = build_url_from_base(bases[i], "/devices/register");
        if (!http.begin(url)) {
            code = -3;
            continue;
        }
        // Short timeouts: HTTPClient defaults to ~10 s which trips the ESP32
        // task watchdog (5 s) and can brick the device on a slow/down manager.
        http.setConnectTimeout(3000);
        http.setTimeout(3000);
        http.addHeader("Content-Type", "application/json");
        if (s_token.length()) {
            http.addHeader("Authorization", String("Bearer ") + s_token);
            http.addHeader("X-EspDisp-Authorization", String("Bearer ") + s_token);
        }
        code = http.POST(payload);
        if (code == 200 || code == 201) {
            resp = http.getString();
            successful_base = bases[i];
            http.end();
            break;
        }
        http.end();
        // 404 from /plugins/... means this is likely the standalone mock.
        if (code != 404) break;
    }
    if (code == 200 || code == 201) {
        JsonDocument r;
        if (deserializeJson(r, resp) == DeserializationError::Ok) {
            if (successful_base.length() && successful_base != s_endpoint) {
                s_endpoint = successful_base;
                save_prefs();
            }
            const char *tok = r["deviceToken"] | "";
            if (tok && *tok) {
                s_token = tok;
                s_auth = AuthState::Provisioned;
                save_prefs();
                net::logf("[mgr] registered ok (token_len=%u)",
                          (unsigned)s_token.length());
            }
            if (r["heartbeat"]["intervalMs"].is<uint32_t>()) {
                s_heartbeat_interval_ms =
                    r["heartbeat"]["intervalMs"].as<uint32_t>();
            } else if (r["heartbeat_interval_ms"].is<uint32_t>()) {
                s_heartbeat_interval_ms =
                    r["heartbeat_interval_ms"].as<uint32_t>();
            }
            if (r["commands"]["pollMs"].is<uint32_t>()) {
                s_command_poll_interval_ms =
                    r["commands"]["pollMs"].as<uint32_t>();
            } else if (r["command_poll_interval_ms"].is<uint32_t>()) {
                s_command_poll_interval_ms =
                    r["command_poll_interval_ms"].as<uint32_t>();
            }
        }
    } else {
        net::logf("[mgr] register -> %d", code);
    }
    s_last_register_ms = millis();
    s_last_register_code = code;
    s_health = (code >= 200 && code < 300) ? HealthState::Heartbeating
                                            : HealthState::Failed;
    return code;
}

void build_status_body(JsonDocument &doc) {
    const auto &id = device_identity::get();
    doc["deviceId"] = id.device_id;
    JsonObject net_o = doc["network"].to<JsonObject>();
    net_o["wifi_up"] = net::wifiUp();
    net_o["state"] = net::wifiStateName();
    net_o["ip"] = net::ipString();
    net_o["rssi"] = net::rssi();
    // F5: current FQDN + OTA address derived from device id.
    String hostname = net::deviceId();
    net_o["hostname"] = hostname;
    net_o["fqdn"] = hostname + ".local";
    net_o["ota_address"] = hostname + ".local:3232";

    JsonObject sk_o = doc["sk"].to<JsonObject>();
    sk_o["state"] = sk::connectionStatus();

    JsonObject ui_o = doc["ui"].to<JsonObject>();
    ui_o["uptime_ms"] = millis();

    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["heap_free_kb"] = (uint32_t)(ESP.getFreeHeap() / 1024);
    mem["psram_free_kb"] =
        (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"] = id.firmware_version;
    fw["build_time"] = id.build_time;
    fw["git_commit"] = id.git_commit;

    JsonObject touch = doc["touch"].to<JsonObject>();
    touch["mode"] = "poll";  // 4848s040 board

    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["version"] = s_applied_config_version;
    cfg["hash"] = s_applied_config_hash;
}

// Apply a single config blob. Returns true on success, false if any
// field was rejected. Persistent NVS writes are made directly; UI
// state (theme, brightness) goes via app::post so the LVGL task owns
// the mutation.
bool apply_config(JsonDocument &cfg) {
    bool ok = true;

    if (cfg["ui"].is<JsonObject>()) {
        JsonObject ui = cfg["ui"].as<JsonObject>();
        if (ui["brightness"].is<int>()) {
            int b = ui["brightness"].as<int>();
            if (b < 0 || b > 255) {
                net::logf("[mgr] reject ui.brightness=%d (out of 0..255)", b);
                ok = false;
            } else {
                app::Command c;
                c.type = app::CommandType::SetBrightness;
                c.i = b;
                app::post(c, 100);
            }
        }
        if (ui["theme"].is<const char *>()) {
            const char *t = ui["theme"].as<const char *>();
            if (strcmp(t, "day") == 0 || strcmp(t, "night") == 0 ||
                strcmp(t, "auto") == 0) {
                app::Command c;
                c.type = app::CommandType::SetTheme;
                strncpy(c.a, t, sizeof(c.a) - 1);
                app::post(c, 100);
            } else {
                net::logf("[mgr] reject ui.theme=%s (unknown)", t);
                ok = false;
            }
        }
    }

    if (cfg["network"].is<JsonObject>()) {
        JsonObject n = cfg["network"].as<JsonObject>();
        // F5: hostname / device_id. Validation: must be non-empty, 1-31
        // chars, alnum + '-'. Apply via the existing `id` console
        // command path so all of BLE/mDNS/OTA hostname stay in sync.
        if (n["hostname"].is<const char *>()) {
            const char *hn = n["hostname"].as<const char *>();
            size_t len = strlen(hn);
            bool valid = len > 0 && len <= 31;
            for (size_t i = 0; valid && i < len; ++i) {
                char c = hn[i];
                valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-';
            }
            if (!valid) {
                net::logf("[mgr] reject network.hostname=%s (invalid)", hn);
                ok = false;
            } else if (n["hostname"] != device_identity::get().device_id) {
                // `id <name>` persists + reboots. We dispatch through
                // the existing handler so behavior is identical to a
                // user-issued change.
                String cmd = String("id ") + hn;
                net::dispatchCommand(cmd);
                net::logf("[mgr] applied network.hostname=%s (rebooting)", hn);
                // Reboot is handled by the id handler; no further work.
                return ok;
            }
        }
    }

    if (cfg["signalk"].is<JsonObject>()) {
        JsonObject sk = cfg["signalk"].as<JsonObject>();
        Preferences p;
        p.begin("sk", false);
        if (sk["host"].is<const char *>()) {
            const char *host = sk["host"].as<const char *>();
            p.putString("host", host);
            net::logf("[mgr] applied sk.host=%s", host);
        }
        if (sk["port"].is<unsigned int>()) {
            uint16_t port = sk["port"].as<unsigned int>();
            p.putUInt("port", port);
            net::logf("[mgr] applied sk.port=%u", port);
        }
        if (sk["token"].is<const char *>()) {
            const char *tok = sk["token"].as<const char *>();
            p.putString("token", tok);
            net::logf("[mgr] applied sk.token (len=%u)",
                      (unsigned)strlen(tok));
        }
        p.end();
        // SK reconnect picks up changes on next ws.begin via the
        // existing sk-reconnect path - schedule it.
        net::dispatchCommand("sk-reconnect");
    }

    return ok;
}

// F6 - post a state line to the OTA job progress endpoint. No-op if
// no endpoint/token (we still try to install locally, but progress
// is informational).
void post_ota_progress(const String &job_id, const char *state,
                       int progress_pct = -1, const char *detail = nullptr) {
    if (!is_provisioned() || job_id.length() == 0) return;
    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id +
                 "/firmware/jobs/" + job_id + "/progress";
    if (!http.begin(url)) return;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_token);
    JsonDocument body;
    body["state"] = state;
    if (progress_pct >= 0) body["progress_pct"] = progress_pct;
    if (detail) body["detail"] = detail;
    String payload;
    serializeJson(body, payload);
    int code = http.POST(payload);
    if (code != 200) {
        net::logf("[mgr-ota] progress %s -> %d", state, code);
    }
    http.end();
}

// Convert a 32-byte SHA-256 digest into 64-char lowercase hex.
void sha256_to_hex(const uint8_t *digest, char out[65]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[64] = 0;
}

void ota_task(void *) {
    esp_task_wdt_delete(NULL);
    s_ota_in_flight = true;
    String job_id = s_ota_job_id;
    String url = s_ota_url;
    String want_sha = s_ota_sha256;
    size_t want_size = s_ota_size;
    const char *failure_detail = nullptr;
    int outcome_code = -1;

    net::logf("[mgr-ota] start job=%s url=%s size=%u",
              job_id.c_str(), url.c_str(), (unsigned)want_size);
    post_ota_progress(job_id, "accepted", 0);

    HTTPClient http;
    if (!http.begin(url)) {
        failure_detail = "http begin failed";
        goto fail;
    }
    http.setConnectTimeout(5000);
    http.setTimeout(15000);  // longer for big binaries
    {
        int code = http.GET();
        if (code != 200) {
            failure_detail = "GET non-200";
            outcome_code = code;
            http.end();
            goto fail;
        }
    }
    {
        size_t actual_size = (size_t)http.getSize();
        if (actual_size == 0 || (want_size && actual_size != want_size)) {
            // Length-unknown servers return -1; only reject if known
            // and mismatching.
            if (actual_size && want_size && actual_size != want_size) {
                failure_detail = "size mismatch";
                http.end();
                goto fail;
            }
        }
        if (!Update.begin(want_size ? want_size : UPDATE_SIZE_UNKNOWN)) {
            failure_detail = "Update.begin failed";
            http.end();
            goto fail;
        }
        post_ota_progress(job_id, "downloading", 0);

        WiFiClient *stream = http.getStreamPtr();
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        uint8_t buf[1024];
        size_t written = 0;
        uint32_t last_progress_ms = millis();
        while (http.connected() && (written < want_size || want_size == 0)) {
            size_t avail = stream->available();
            if (!avail) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            size_t n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (!n) break;
            mbedtls_sha256_update(&sha, buf, n);
            size_t w = Update.write(buf, n);
            if (w != n) {
                failure_detail = "Update.write short";
                mbedtls_sha256_free(&sha);
                http.end();
                Update.abort();
                goto fail;
            }
            written += n;
            // Periodic progress every ~2 s while downloading.
            uint32_t now = millis();
            if (now - last_progress_ms > 2000 && want_size > 0) {
                int pct = (int)((written * 100UL) / want_size);
                post_ota_progress(job_id, "downloading", pct);
                last_progress_ms = now;
            }
        }
        http.end();

        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        char got_sha[65];
        sha256_to_hex(digest, got_sha);

        post_ota_progress(job_id, "verifying", 100);
        if (want_sha.length() == 64) {
            String w(want_sha);
            w.toLowerCase();
            if (w != String(got_sha)) {
                net::logf("[mgr-ota] sha mismatch want=%s got=%s",
                          w.c_str(), got_sha);
                failure_detail = "sha256 mismatch";
                Update.abort();
                goto fail;
            }
        }

        post_ota_progress(job_id, "installing", 100);
        if (!Update.end(true)) {
            failure_detail = "Update.end failed";
            goto fail;
        }
        if (Update.hasError()) {
            failure_detail = "Update.hasError";
            goto fail;
        }
    }

    post_ota_progress(job_id, "rebooting", 100);
    net::logf("[mgr-ota] job=%s installed, rebooting", job_id.c_str());
    s_ota_in_flight = false;
    delay(500);
    ESP.restart();
    // not reached
    vTaskDelete(NULL);
    return;

fail:
    net::logf("[mgr-ota] FAILED job=%s detail=%s code=%d",
              job_id.c_str(),
              failure_detail ? failure_detail : "?",
              outcome_code);
    post_ota_progress(job_id, "failed", -1,
                      failure_detail ? failure_detail : "unknown");
    s_ota_in_flight = false;
    s_ota_task = nullptr;
    vTaskDelete(NULL);
}

// F4 - execute a single queued command. Returns one of:
//   "ok" | "unsupported_command" | "invalid_payload" | "failed"
const char *execute_command(const char *type, JsonObject payload) {
    if (!type || !*type) return "invalid_payload";

    if (strcmp(type, "screen.set") == 0) {
        const char *id = payload["screen"] | "";
        if (!*id) id = payload["id"] | "";
        if (!*id) return "invalid_payload";
        app::Command c;
        c.type = app::CommandType::ShowScreen;
        strncpy(c.a, id, sizeof(c.a) - 1);
        c.t_post_us = micros();
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "theme.set") == 0) {
        const char *t = payload["theme"] | "";
        if (strcmp(t, "day") && strcmp(t, "night") && strcmp(t, "auto")) {
            return "invalid_payload";
        }
        app::Command c;
        c.type = app::CommandType::SetTheme;
        strncpy(c.a, t, sizeof(c.a) - 1);
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "brightness.set") == 0) {
        if (!payload["value"].is<int>()) return "invalid_payload";
        int v = payload["value"].as<int>();
        if (v < 0 || v > 255) return "invalid_payload";
        app::Command c;
        c.type = app::CommandType::SetBrightness;
        c.i = v;
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "config.reload") == 0) {
        s_config_fetch_pending = true;
        return "ok";
    }
    if (strcmp(type, "reboot") == 0) {
        net::logf("[mgr] reboot requested by manager");
        // Defer reboot ~1 s so we can POST the ack first.
        app::Command c;
        c.type = app::CommandType::Reboot;
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "firmware.update") == 0) {
        if (s_ota_in_flight) return "busy";
        const char *url = payload["url"] | "";
        const char *sha = payload["sha256"] | "";
        const char *ver = payload["version"] | "";
        const char *job_id = payload["jobId"] | "";
        if (!*job_id) job_id = payload["job_id"] | "";
        size_t size = payload["size"] | 0;
        if (!*url || !*job_id) return "invalid_payload";
        if (size && size < 1024) return "invalid_payload";  // sanity
        if (*sha && strlen(sha) != 64) return "invalid_payload";
        s_ota_job_id = job_id;
        s_ota_url = url;
        s_ota_sha256 = sha;
        s_ota_version = ver;
        s_ota_size = size;
        if (xTaskCreatePinnedToCore(ota_task, "mgr-ota", 8192, nullptr, 1,
                                    &s_ota_task, 0) != pdPASS) {
            return "failed";
        }
        return "ok";
    }
    if (strcmp(type, "beep") == 0) {
        uint32_t ms = payload["duration_ms"] | 50;
        beeper::beep_short(ms);
        return "ok";
    }
    return "unsupported_command";
}

void ack_command(const String &cmd_id, const char *result) {
    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id +
                 "/commands/" + cmd_id + "/ack";
    if (!http.begin(url)) return;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_token);
    JsonDocument body;
    body["result"] = result;
    String payload;
    serializeJson(body, payload);
    int code = http.POST(payload);
    if (code != 200) {
        net::logf("[mgr] ack %s -> %d", cmd_id.c_str(), code);
    }
    http.end();
}

int poll_commands() {
    if (!is_provisioned()) return -1;
    if (WiFi.status() != WL_CONNECTED) return -2;

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id +
                 "/commands";
    if (!http.begin(url)) return -3;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Authorization", String("Bearer ") + s_token);
    int code = http.GET();
    if (code == 200) {
        String resp = http.getString();
        http.end();  // close before any nested HTTP calls (acks)
        JsonDocument r;
        if (deserializeJson(r, resp) != DeserializationError::Ok) return code;
        JsonArray cmds = r["commands"].as<JsonArray>();
        for (JsonObject cmd : cmds) {
            String cid = cmd["id"] | "";
            const char *type = cmd["type"] | "";
            JsonObject payload = cmd["payload"].as<JsonObject>();
            const char *result = execute_command(type, payload);
            net::logf("[mgr] cmd %s type=%s -> %s",
                      cid.c_str(), type, result);
            if (cid.length()) ack_command(cid, result);
        }
        return 200;
    }
    http.end();
    if (code < 200 || code >= 300) {
        net::logf("[mgr] commands fetch -> %d", code);
    }
    return code;
}

int fetch_config() {
    if (!is_provisioned()) return -1;
    if (WiFi.status() != WL_CONNECTED) return -2;

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id +
                 "/config";
    if (!http.begin(url)) return -3;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Authorization", String("Bearer ") + s_token);
    int code = http.GET();
    if (code == 200) {
        String resp = http.getString();
        JsonDocument r;
        if (deserializeJson(r, resp) == DeserializationError::Ok) {
            String new_version = r["version"] | "";
            String new_hash = r["hash"] | "";
            JsonObject cfg_src;
            if (r["config"].is<JsonObject>()) {
                cfg_src = r["config"].as<JsonObject>();
            } else {
                cfg_src = r.as<JsonObject>();
            }
            if (!new_version.length() && r["version"].is<int>()) {
                new_version = String(r["version"].as<int>());
            }
            if (cfg_src) {
                JsonDocument cfg;
                cfg.set(cfg_src);
                // Spec 19 D2 -> D5: build a RenderPlan from the
                // config so diagnostics can show it. Failures don't
                // block the legacy apply path below; the device
                // continues to honor ui/sk/network blocks even when
                // the spec-19 layout/widgets are malformed.
                board::Geometry g = board::geometry();
                manager_config::RenderPlan plan;
                manager_config::ParseError perr;
                if (manager_config::parse(cfg.as<JsonObjectConst>(),
                                          g.width_px, g.height_px,
                                          plan, perr)) {
                    s_render_plan = plan;
                    s_render_plan_valid = true;
                    net::logf("[mgr] render plan: widgets=%u screens=%u "
                              "variant=%s",
                              (unsigned)plan.widget_count,
                              (unsigned)plan.screen_count,
                              plan.layout_variant[0] ? plan.layout_variant
                                                     : "(none)");
                } else {
                    net::logf("[mgr] render plan parse failed: %s at %s",
                              manager_config::parse_code_to_string(perr.code),
                              perr.path[0] ? perr.path : "(root)");
                }
                bool ok = apply_config(cfg);
                if (ok) {
                    lock_state();
                    s_applied_config_version = new_version;
                    s_applied_config_hash = new_hash;
                    save_prefs();
                    unlock_state();
                    net::logf("[mgr] config applied: version=%s hash=%s",
                              new_version.c_str(), new_hash.c_str());
                } else {
                    net::logf("[mgr] config %s had invalid fields; "
                              "applied valid subset only (not persisted)",
                              new_version.c_str());
                }
            }
        }
    } else {
        net::logf("[mgr] config fetch -> %d", code);
    }
    http.end();
    return code;
}

int do_heartbeat() {
    if (!is_provisioned()) return -1;
    if (WiFi.status() != WL_CONNECTED) return -2;

    JsonDocument body;
    build_status_body(body);
    String payload;
    serializeJson(body, payload);

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id +
                 "/status";
    if (!http.begin(url)) return -3;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + s_token);
    int code = http.POST(payload);
    if (code == 401 || code == 403) {
        net::logf("[mgr] heartbeat auth failed (%d) - will re-register", code);
        lock_state();
        s_token = "";
        s_auth = AuthState::Unprovisioned;
        s_force_register = true;
        unlock_state();
    } else if (code == 200) {
        // F3: check if the server wants a config update.
        String resp = http.getString();
        JsonDocument r;
        if (deserializeJson(r, resp) == DeserializationError::Ok) {
            String want_ver = r["desiredConfig"]["version"] | "";
            String want_hash = r["desiredConfig"]["hash"] | "";
            if (!want_ver.length()) want_ver = r["desired_config_version"] | "";
            if (!want_hash.length()) want_hash = r["desired_config_hash"] | "";
            lock_state();
            s_desired_config_version = want_ver;
            s_desired_config_hash = want_hash;
            bool drift = (want_hash.length() && want_hash != s_applied_config_hash) ||
                         (want_ver.length() && want_ver != s_applied_config_version);
            unlock_state();
            if (drift) {
                net::logf("[mgr] config drift: have=%s want=%s -> fetching",
                          s_applied_config_version.c_str(), want_ver.c_str());
                s_config_fetch_pending = true;
            }
        }
    } else if (code < 200 || code >= 300) {
        net::logf("[mgr] heartbeat -> %d", code);
    }
    http.end();
    s_last_heartbeat_ms = millis();
    s_last_heartbeat_code = code;

    // Spec 17 §10 / 18 §11: after a post-OTA boot, the first successful
    // heartbeat triggers a /firmware/confirm POST so the plugin can
    // mark its OTA job complete. We don't have the original job id at
    // boot, so the body just carries the new build's identity - the
    // plugin correlates by deviceId + version+hash.
    if (code >= 200 && code < 300 && s_ota_confirm_pending) {
        HTTPClient hc;
        String confirm_url = build_url("/devices/") +
                             device_identity::get().device_id +
                             "/firmware/confirm";
        if (hc.begin(confirm_url)) {
            hc.setConnectTimeout(3000);
            hc.setTimeout(3000);
            hc.addHeader("Content-Type", "application/json");
            hc.addHeader("Authorization", String("Bearer ") + s_token);
            JsonDocument cbody;
            const auto &id = device_identity::get();
            cbody["version"] = id.firmware_version;
            cbody["build_time"] = id.build_time;
            cbody["git_commit"] = id.git_commit;
            String payload;
            serializeJson(cbody, payload);
            int cc = hc.POST(payload);
            net::logf("[mgr] /firmware/confirm -> %d", cc);
            hc.end();
            if (cc >= 200 && cc < 300) s_ota_confirm_pending = false;
        }
    }
    return code;
}

void worker(void *) {
    // Opt out of the task watchdog: HTTPClient can block for up to the
    // configured timeout (3 s now) and we don't want a single slow
    // POST to brick the device.
    esp_task_wdt_delete(NULL);
    uint32_t next_register_attempt_ms = 0;
    uint32_t next_heartbeat_ms = 0;
    uint32_t next_command_poll_ms = 0;
    for (;;) {
        // Snapshot mutable state under the mutex so CLI updates can't
        // race the HTTP path mid-call.
        lock_state();
        bool have_endpoint = s_endpoint.length() > 0;
        bool prov = is_provisioned();
        bool force = s_force_register;
        unlock_state();

        if (!have_endpoint) {
            s_health = HealthState::Idle;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        uint32_t now = millis();
        if (force || !prov) {
            if (now >= next_register_attempt_ms) {
                int rc = do_register();
                if (rc >= 200 && rc < 300) {
                    s_force_register = false;
                    next_heartbeat_ms = now + 1000;
                } else {
                    // Back off: longer delay on transport errors to
                    // avoid pounding an unreachable peer.
                    next_register_attempt_ms = now +
                        (rc < 0 ? 10000 : 5000);
                }
            }
        }
        if (prov && now >= next_heartbeat_ms) {
            do_heartbeat();
            next_heartbeat_ms = millis() + s_heartbeat_interval_ms;
        }
        if (prov && s_config_fetch_pending) {
            s_config_fetch_pending = false;
            fetch_config();
        }
        if (prov && now >= next_command_poll_ms) {
            poll_commands();
            next_command_poll_ms = millis() + s_command_poll_interval_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void setup() {
    if (!s_state_mtx) s_state_mtx = xSemaphoreCreateMutex();
    load_prefs();
    // F6: if we just booted from a new OTA partition, mark it valid.
    // ESP32 keeps the new image in PENDING_VERIFY until something
    // calls esp_ota_mark_app_valid; without that a hard reset rolls
    // back. We do it unconditionally here - if the device booted to
    // the point of running setup(), it's healthy enough to keep.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_ota_confirm_pending = true;
            net::logf("[mgr] post-OTA boot: marked partition valid; "
                      "/firmware/confirm pending");
        }
    }
    net::logf("[mgr] %s endpoint=%s token=%s",
              s_endpoint.length() ? "configured" : "idle",
              s_endpoint.c_str(),
              s_token.length() ? "set" : "none");
    if (!s_task) {
        xTaskCreatePinnedToCore(worker, "mgr", 6144, nullptr, 1, &s_task, 0);
    }
}

bool is_provisioned() {
    return s_endpoint.length() > 0 && s_token.length() > 0;
}

Status status() {
    Status s;
    s.auth = s_auth;
    s.health = s_health;
    s.endpoint = s_endpoint;
    s.has_token = s_token.length() > 0;
    s.last_register_ms = s_last_register_ms;
    s.last_register_code = s_last_register_code;
    s.last_heartbeat_ms = s_last_heartbeat_ms;
    s.last_heartbeat_code = s_last_heartbeat_code;
    s.heartbeat_interval_ms = s_heartbeat_interval_ms;
    s.command_poll_interval_ms = s_command_poll_interval_ms;
    s.device_id = device_identity::get().device_id;
    s.config_version = s_applied_config_version;
    s.config_hash = s_applied_config_hash;
    return s;
}

bool handleSerialCommand(const String &line) {
    if (!line.startsWith("manager") && !line.startsWith("mgr-")) {
        return false;
    }

    if (line == "manager-status" || line == "manager") {
        Status st = status();
        net::logf("[mgr] auth=%s health=%s endpoint=%s token=%s",
                  st.auth == AuthState::Provisioned ? "provisioned" : "unprov",
                  st.health == HealthState::Heartbeating ? "hb"
                    : st.health == HealthState::Registering ? "reg"
                    : st.health == HealthState::Failed ? "failed" : "idle",
                  st.endpoint.length() ? st.endpoint.c_str() : "(none)",
                  st.has_token ? "set" : "none");
        net::logf("[mgr] last_register=%dms ago code=%d  "
                  "last_hb=%dms ago code=%d",
                  (int)(millis() - st.last_register_ms),
                  st.last_register_code,
                  (int)(millis() - st.last_heartbeat_ms),
                  st.last_heartbeat_code);
        return true;
    }
    if (line.startsWith("manager-register ")) {
        String url = line.substring(17);
        url.trim();
        int first_space = url.indexOf(' ');
        if (first_space > 0) {
            String host = url.substring(0, first_space);
            String port = url.substring(first_space + 1);
            host.trim();
            port.trim();
            if (!host.startsWith("http://") && !host.startsWith("https://")) {
                url = String("http://") + host + ":" + port;
            }
        } else if (!url.startsWith("http://") && !url.startsWith("https://")) {
            url = String("http://") + url;
        }
        if (url.length() == 0) {
            net::logf("[mgr] usage: manager-register <http://host:port>");
            return true;
        }
        lock_state();
        s_endpoint = url;
        if (!s_token.length()) s_auth = AuthState::Unprovisioned;
        save_prefs();
        s_force_register = true;
        unlock_state();
        net::logf("[mgr] endpoint set, will register on next tick");
        return true;
    }
    if (line.startsWith("manager-token ")) {
        String tok = line.substring(14);
        tok.trim();
        if (tok == "clear") {
            s_token = "";
            s_auth = AuthState::Unprovisioned;
        } else {
            s_token = tok;
            s_auth = AuthState::Provisioned;
        }
        save_prefs();
        net::logf("[mgr] token %s",
                  s_token.length() ? "saved" : "cleared");
        return true;
    }
    if (line == "manager-forget") {
        lock_state();
        s_endpoint = "";
        s_token = "";
        s_auth = AuthState::Unprovisioned;
        save_prefs();
        unlock_state();
        net::logf("[mgr] forgotten");
        return true;
    }
    if (line == "manager-discover") {
        // F2 mDNS discovery placeholder. mDNS query for
        // _espdisp-mgmt._tcp.local. lands in a follow-up; for now
        // just log so the CLI is wired.
        net::logf("[mgr] mDNS discovery TBD (use manager-register <url>)");
        return true;
    }
    // ---- spec 19 D7 diagnostic commands -------------------------------
    if (line == "manager-layout") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        net::logf("[mgr] layout variant=%s widgets=%u screens=%u "
                  "(%ux%u) hash=%s",
                  s_render_plan.layout_variant[0] ? s_render_plan.layout_variant
                                                  : "(none)",
                  (unsigned)s_render_plan.widget_count,
                  (unsigned)s_render_plan.screen_count,
                  (unsigned)s_render_plan.display_width,
                  (unsigned)s_render_plan.display_height,
                  s_applied_config_hash.length() ?
                      s_applied_config_hash.c_str() : "(none)");
        for (uint8_t i = 0; i < s_render_plan.screen_count; ++i) {
            const auto &sc = s_render_plan.screens[i];
            net::logf("[mgr]   screen[%u] id=%s tiles=%u",
                      (unsigned)i, sc.id, (unsigned)sc.tile_count);
        }
        return true;
    }
    if (line == "manager-widgets") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        for (uint8_t i = 0; i < s_render_plan.widget_count; ++i) {
            const auto &w = s_render_plan.widgets[i];
            net::logf("[mgr] widget[%u] id=%s type=%s path=%s "
                      "title=%s unit=%s prec=%u fs=%u",
                      (unsigned)i, w.id,
                      manager_config::widget_type_to_string(w.type),
                      w.path, w.title, w.unit,
                      (unsigned)w.precision,
                      (unsigned)(w.style.font_size ? w.style.font_size
                                                  : w.style.value_font_size));
        }
        return true;
    }
    if (line == "manager-config-dump") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        JsonDocument out;
        out["layout_variant"] = s_render_plan.layout_variant;
        out["widget_variant"] = s_render_plan.widget_variant;
        out["display"]["width"] = s_render_plan.display_width;
        out["display"]["height"] = s_render_plan.display_height;
        out["widget_count"] = s_render_plan.widget_count;
        out["screen_count"] = s_render_plan.screen_count;
        out["config_version"] = s_applied_config_version;
        out["config_hash"] = s_applied_config_hash;
        String s;
        serializeJson(out, s);
        net::logf("[mgr] config: %s", s.c_str());
        return true;
    }
    if (line == "font-dump") {
        net::logf("[font] compiled sizes:");
        for (size_t i = 0; i < font_resolver::DEFAULT_COUNT; ++i) {
            net::logf("[font]   [%u] = %u",
                      (unsigned)i,
                      (unsigned)font_resolver::DEFAULT_SIZES[i]);
        }
        // Demo a few resolve calls.
        uint16_t probes[] = {10, 16, 22, 30, 42, 80};
        for (uint16_t p : probes) {
            net::logf("[font]   resolve(%u) -> %u",
                      (unsigned)p,
                      (unsigned)font_resolver::resolve_default(p));
        }
        return true;
    }
    net::logf("[mgr] usage: manager-status | manager-register <url> | "
              "manager-token <jwt|clear> | manager-forget | manager-discover "
              "| manager-layout | manager-widgets | manager-config-dump "
              "| font-dump");
    return true;
}

}  // namespace manager
