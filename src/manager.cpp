#include "manager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "app_events.h"
#include "device_identity.h"
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
String s_desired_config_version = "";
String s_desired_config_hash = "";
volatile bool s_config_fetch_pending = false;
volatile uint32_t s_last_register_ms = 0;
volatile int s_last_register_code = 0;
volatile uint32_t s_last_heartbeat_ms = 0;
volatile int s_last_heartbeat_code = 0;
TaskHandle_t s_task = nullptr;
volatile bool s_force_register = false;
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

    HTTPClient http;
    String url = build_url("/devices/register");
    if (!http.begin(url)) {
        s_health = HealthState::Failed;
        return -3;
    }
    // Short timeouts: HTTPClient defaults to ~10 s which trips the ESP32
    // task watchdog (5 s) and can brick the device on a slow/down manager.
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    if (code == 200 || code == 201) {
        String resp = http.getString();
        JsonDocument r;
        if (deserializeJson(r, resp) == DeserializationError::Ok) {
            const char *tok = r["deviceToken"] | "";
            if (tok && *tok) {
                s_token = tok;
                s_auth = AuthState::Provisioned;
                save_prefs();
                net::logf("[mgr] registered ok (token_len=%u)",
                          (unsigned)s_token.length());
            }
            if (r["heartbeat_interval_ms"].is<uint32_t>()) {
                s_heartbeat_interval_ms =
                    r["heartbeat_interval_ms"].as<uint32_t>();
            }
            if (r["command_poll_interval_ms"].is<uint32_t>()) {
                s_command_poll_interval_ms =
                    r["command_poll_interval_ms"].as<uint32_t>();
            }
        }
    } else {
        net::logf("[mgr] register -> %d", code);
    }
    http.end();
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

// F4 - execute a single queued command. Returns one of:
//   "ok" | "unsupported_command" | "invalid_payload" | "failed"
const char *execute_command(const char *type, JsonObject payload) {
    if (!type || !*type) return "invalid_payload";

    if (strcmp(type, "screen.set") == 0) {
        const char *id = payload["id"] | "";
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
    if (strcmp(type, "beep") == 0) {
        // No beeper on this board; log and ack ok per spec.
        uint32_t ms = payload["duration_ms"] | 50;
        net::logf("[mgr] beep %lu ms (no hardware - logged only)",
                  (unsigned long)ms);
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
            if (r["config"].is<JsonObject>()) {
                JsonDocument cfg;
                cfg.set(r["config"]);
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
            String want_ver = r["desired_config_version"] | "";
            String want_hash = r["desired_config_hash"] | "";
            lock_state();
            s_desired_config_version = want_ver;
            s_desired_config_hash = want_hash;
            bool drift = want_ver.length() && want_ver != s_applied_config_version;
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
        if (url.length() == 0) {
            net::logf("[mgr] usage: manager-register <http://host:port>");
            return true;
        }
        lock_state();
        s_endpoint = url;
        s_token = "";
        s_auth = AuthState::Unprovisioned;
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
    net::logf("[mgr] usage: manager-status | manager-register <url> | "
              "manager-token <jwt|clear> | manager-forget | manager-discover");
    return true;
}

}  // namespace manager
