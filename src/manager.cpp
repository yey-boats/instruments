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
    p.end();
    s_auth = s_token.length() ? AuthState::Provisioned
                              : AuthState::Unprovisioned;
}

void save_prefs() {
    Preferences p;
    p.begin(NS, false);
    p.putString("endpoint", s_endpoint);
    p.putString("token", s_token);
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
    cfg["version"] = "v0";   // F3 will populate this
    cfg["hash"] = "";
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
        // Treat auth failure as needing re-register, but never as
        // fatal (per spec 17 §3 rules).
        net::logf("[mgr] heartbeat auth failed (%d) - will re-register", code);
        s_token = "";
        s_auth = AuthState::Unprovisioned;
        s_force_register = true;
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
