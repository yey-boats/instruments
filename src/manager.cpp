#include "manager.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>

#include "app_events.h"
#include "beeper.h"
#include "board.h"
#include "boat_data.h"
#include "device_identity.h"
#include "font_resolver.h"
#include "error_log.h"
#include "hostname_check.h"
#include "sources_check.h"
#include "ui_config_check.h"
#include "manager_config.h"
#include "manager_screens.h"
#include "manager_url.h"
#include "net.h"

// Spec 17 §8 touch.mode: the toggle lives in main.cpp because that's
// where the GT911 worker + INT pin are owned. Forward-declared with C
// linkage to match main.cpp's extern "C" block.
extern "C" bool main_set_touch_mode(const char *mode);
#include "signalk.h"
#include "source_nmea2000.h"
#include "source_nmea_wifi.h"
#include "ui_data.h"
#include "ui_screens.h"

namespace manager {

namespace {

constexpr const char *NS = "mgr";
constexpr uint32_t DEFAULT_HEARTBEAT_MS = 30000;
constexpr uint32_t DEFAULT_COMMAND_POLL_MS = 10000;

String s_endpoint;
String s_token;       // device/dev/provision token sent as X-EspDisp-Authorization
String s_sk_token;    // SignalK server bearer token used to pass SK security
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
// Spec 17 §11 local diagnostics: surface command queue health so the
// operator can see whether the device is processing or stuck.
volatile uint8_t s_pending_cmd_count = 0;
volatile uint32_t s_last_cmd_ms = 0;
String s_last_cmd_id;
String s_last_cmd_type;
String s_last_cmd_result;
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
    s_sk_token = p.getString("sk_token", "");
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
    p.putString("sk_token", s_sk_token);
    p.putString("cfg_ver", s_applied_config_version);
    p.putString("cfg_hash", s_applied_config_hash);
    p.end();
}

// Hard caps on response bodies we'll accept from the plugin. A hostile
// or buggy server could return arbitrarily large payloads; reading
// everything into a String would OOM the device. These are generous
// for legitimate payloads but bounded.
constexpr int MAX_DISCOVERY_BYTES = 4 * 1024;
constexpr int MAX_HEARTBEAT_RESP_BYTES = 4 * 1024;
constexpr int MAX_CONFIG_BYTES = 32 * 1024;
constexpr int MAX_COMMANDS_BYTES = 8 * 1024;

// Returns true iff the Content-Length header (if any) is within `cap`.
// HTTPClient::getSize() returns -1 when the server omits Content-Length;
// in that case we accept the read but rely on the small timeouts to
// bound it.
bool resp_within_cap(HTTPClient &http, int cap, const char *who) {
    int sz = http.getSize();
    if (sz > cap) {
        net::logf("[mgr] %s response too large (%d > %d) - dropping",
                  who, sz, cap);
        return false;
    }
    return true;
}

// Spec 17 §5 helper: logf an error AND drop it into the recent-errors
// ring so the next heartbeat surfaces it. Identical signature to
// net::logf; format result is bounded by error_log::MAX_MESSAGE.
void record_error(const char *fmt, ...) {
    char buf[error_log::MAX_MESSAGE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    net::logf("%s", buf);
    error_log::push((uint32_t)millis(), buf);
}

// Compose Authorization (SK security) + X-EspDisp-Authorization (plugin auth)
// for any plugin HTTP request. SK security needs a server-issued token in the
// standard Authorization header; the plugin itself reads X-EspDisp-Authorization
// first (see signalk-espdisp-manager index.js authFrom). When the device only
// has one token (e.g. talking to the standalone mock), both headers carry it.
void add_auth_headers(HTTPClient &http) {
    if (s_sk_token.length()) {
        http.addHeader("Authorization", String("Bearer ") + s_sk_token);
    } else if (s_token.length()) {
        http.addHeader("Authorization", String("Bearer ") + s_token);
    }
    if (s_token.length()) {
        http.addHeader("X-EspDisp-Authorization", String("Bearer ") + s_token);
    }
}

// Firmware-facing wrappers around manager_url::* (which are pure
// std::string and host-tested in test/test_manager_url). Keeping the
// Arduino String signatures here means callers don't have to convert
// at every site.
String build_url(const char *path) {
    return String(manager_url::join_url(s_endpoint.c_str(), path).c_str());
}

String build_url_from_base(const String &base, const char *path) {
    return String(manager_url::join_url(base.c_str(), path).c_str());
}

String plugin_base_from_root(const String &endpoint) {
    return String(manager_url::plugin_base_from_root(endpoint.c_str()).c_str());
}

bool endpoint_has_path(const String &endpoint) {
    return manager_url::endpoint_has_path(endpoint.c_str());
}

// GET /.well-known/espdisp-management for a given base. Best-effort: if
// the plugin advertises intervals or a basePath override, apply them.
// Returns the HTTP code (or negative on transport error).
int fetch_discovery(const String &base, String *out_base_path = nullptr) {
    HTTPClient http;
    String url = build_url_from_base(base, "/.well-known/espdisp-management");
    if (!http.begin(url)) return -3;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    add_auth_headers(http);
    int code = http.GET();
    if (code == 200 && resp_within_cap(http, MAX_DISCOVERY_BYTES, "discovery")) {
        String body = http.getString();
        JsonDocument d;
        if (deserializeJson(d, body) == DeserializationError::Ok) {
            if (d["intervals"]["heartbeatMs"].is<uint32_t>()) {
                s_heartbeat_interval_ms =
                    d["intervals"]["heartbeatMs"].as<uint32_t>();
            }
            if (d["intervals"]["commandPollMs"].is<uint32_t>()) {
                s_command_poll_interval_ms =
                    d["intervals"]["commandPollMs"].as<uint32_t>();
            }
            if (out_base_path && d["basePath"].is<const char *>()) {
                *out_base_path = d["basePath"].as<const char *>();
            }
            net::logf("[mgr] discovery: hb=%ums poll=%ums",
                      (unsigned)s_heartbeat_interval_ms,
                      (unsigned)s_command_poll_interval_ms);
        }
    }
    http.end();
    return code;
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
        // Probe discovery first - if it 200s, pull intervals + confirm
        // the base. We still try POST register even on non-200 (some
        // deployments lock the well-known endpoint).
        fetch_discovery(bases[i]);

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
        add_auth_headers(http);
        code = http.POST(payload);
        if (code == 200 || code == 201) {
            if (resp_within_cap(http, MAX_HEARTBEAT_RESP_BYTES, "register")) {
                resp = http.getString();
                successful_base = bases[i];
            }
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
        record_error("[mgr] register -> %d", code);
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
    doc["device_id"] = id.device_id;

    board::Geometry g = board::geometry();
    Preferences ui_prefs;
    ui_prefs.begin("ui", true);
    String theme_name = ui_prefs.getString("theme", "night");
    ui_prefs.end();

    JsonObject net_o = doc["network"].to<JsonObject>();
    net_o["wifi_up"] = net::wifiUp();
    net_o["state"] = net::wifiStateName();
    net_o["ip"] = net::ipString();
    net_o["rssi"] = net::rssi();
    // F5: current FQDN + OTA address derived from device id.
    String hostname = net::deviceId();
    net_o["hostname"] = hostname;
    net_o["domain"] = "local";
    net_o["fqdn"] = hostname + ".local";
    net_o["ota_address"] = hostname + ".local:3232";
    JsonObject mdns = net_o["mdns"].to<JsonObject>();
    mdns["enabled"] = true;
    JsonArray services = mdns["services"].to<JsonArray>();
    services.add("_espdisp._tcp");
    services.add("_arduino._tcp");

    JsonObject sk_o = doc["sk"].to<JsonObject>();
    String sk_state = sk::connectionStatus();
    sk_o["state"] = sk_state;
    JsonObject signalk_o = doc["signalk"].to<JsonObject>();
    signalk_o["connected"] = sk_state == "live";
    signalk_o["state"] = sk_state;

    JsonObject ui_o = doc["ui"].to<JsonObject>();
    ui_o["uptime_ms"] = millis();
    ui_o["screen"] = ui::current_id();
    ui_o["theme"] = theme_name;
    ui_o["brightness"] = ui::brightness();
    ui_o["layoutVariant"] = s_render_plan.layout_variant;
    ui_o["widgetVariant"] = s_render_plan.widget_variant;
    ui_o["widgetConfigHash"] = s_applied_config_hash;

    JsonObject display_o = doc["display"].to<JsonObject>();
    display_o["width"] = g.width_px;
    display_o["height"] = g.height_px;
    display_o["rotation"] = g.rotation;
    display_o["brightness"] = ui::brightness();

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

    nmea_wifi::Status nw = nmea_wifi::status();
    JsonObject n0183 = doc["nmea0183Wifi"].to<JsonObject>();
    n0183["enabled"] = nw.enabled;
    n0183["mode"] = nw.proto == nmea_wifi::Protocol::Udp ? "udp" : "tcp";
    n0183["host"] = nw.host;
    n0183["port"] = nw.port;
    n0183["connected"] = nw.connected;
    n0183["bytesIn"] = nw.bytes_in;
    n0183["sentencesOk"] = nw.sentences_ok;
    n0183["sentencesBad"] = nw.sentences_bad;
    n0183["lastRxMs"] = nw.last_rx_ms;

    nmea2000::Status n2k = nmea2000::status();
    JsonObject n2k_o = doc["nmea2000"].to<JsonObject>();
    n2k_o["compiledIn"] = n2k.compiled_in;
    n2k_o["enabled"] = n2k.enabled;
    n2k_o["framesRx"] = n2k.frames_rx;
    n2k_o["pgnsDecoded"] = n2k.pgns_decoded;
    n2k_o["lastRxMs"] = n2k.last_rx_ms;

    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["enabled"] = true;
    ota["mode"] = "arduino-ota";
    ota["address"] = hostname + ".local";
    ota["port"] = 3232;
    ota["passwordSet"] = false;
    ota["pullInFlight"] = s_ota_in_flight;
    ota["pendingConfirm"] = s_ota_confirm_pending;

    Preferences web_prefs;
    web_prefs.begin("web", true);
    JsonObject web_auth = doc["webAuth"].to<JsonObject>();
    web_auth["enabled"] = web_prefs.getUChar("auth", 0) != 0;
    web_auth["username"] = web_prefs.getString("user", "espdisp");
    web_auth["passwordSet"] = web_prefs.getString("pass", "").length() > 0;
    web_prefs.end();

    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["version"] = s_applied_config_version;
    cfg["hash"] = s_applied_config_hash;
    cfg["applied"] = s_applied_config_hash.length() > 0;

    // Spec 17 §5 recent errors. The ring is populated by callers
    // logging via net::push_error (or directly in tests). We emit in
    // chronological order; oldest first.
    JsonArray errs = doc["errors"].to<JsonArray>();
    error_log::Entry buf[error_log::MAX_ENTRIES];
    size_t n = error_log::copy(buf, error_log::MAX_ENTRIES);
    for (size_t i = 0; i < n; ++i) {
        JsonObject e = errs.add<JsonObject>();
        e["t_ms"] = buf[i].timestamp_ms;
        e["msg"] = buf[i].message;
    }
}

// Apply a single config blob. Returns true on success, false if any
// field was rejected. Persistent NVS writes are made directly; UI
// state (theme, brightness) goes via app::post so the LVGL task owns
// the mutation.
bool apply_config(JsonDocument &cfg) {
    // Spec 17 §6 dependency order:
    //   1. network hostname/domain   (may reboot - terminal)
    //   2. SignalK target
    //   3. source settings           (TBD; not implemented yet)
    //   4. UI (theme/brightness/layout)
    //   5. debug                      (webAuth here as a proxy)
    bool ok = true;

    // ---- 1. network -------------------------------------------------------
    if (cfg["network"].is<JsonObject>()) {
        JsonObject n = cfg["network"].as<JsonObject>();
        // F5: hostname / device_id. Validation in hostname_check::is_valid.
        // Apply via the existing `id` console command so BLE/mDNS/OTA
        // hostname stay in sync.
        if (n["hostname"].is<const char *>()) {
            const char *hn = n["hostname"].as<const char *>();
            if (!hostname_check::is_valid(hn)) {
                record_error("[mgr] reject network.hostname=%s (invalid)", hn);
                ok = false;
            } else if (n["hostname"] != device_identity::get().device_id) {
                String cmd = String("id ") + hn;
                net::dispatchCommand(cmd);
                net::logf("[mgr] applied network.hostname=%s (rebooting)", hn);
                // Reboot is handled by the `id` dispatcher; further blocks
                // can't matter because we won't outlive the reboot. Return
                // early so we don't enqueue ui/sk work that the reboot
                // would only kill.
                return ok;
            }
        }
    }

    // ---- 2. SignalK target ------------------------------------------------
    if (cfg["signalk"].is<JsonObject>()) {
        JsonObject sk = cfg["signalk"].as<JsonObject>();
        bool changed = false;
        bool reset_to_auto = false;
        Preferences p;
        p.begin("sk", false);
        if (sk["host"].is<const char *>()) {
            const char *host = sk["host"].as<const char *>();
            bool use_mdns = sk["useMdns"] | false;
            bool manager_default =
                use_mdns && strcmp(host, "signalk.local") == 0;
            if (manager_default) {
                // The SignalK plugin advertises signalk.local as a service
                // discovery hint. Persisting it as a manual target disables
                // the firmware's mDNS discovery path and can overwrite a
                // working local IP configured after flashing.
                String current_host = p.getString("host", "");
                if (current_host == "signalk.local") {
                    p.remove("host");
                    p.remove("port");
                    reset_to_auto = true;
                    net::logf("[mgr] cleared persisted sk.host=%s; use mDNS",
                              host);
                } else {
                    net::logf("[mgr] ignored default sk.host=%s (useMdns=true)",
                              host);
                }
            } else {
                p.putString("host", host);
                changed = true;
                net::logf("[mgr] applied sk.host=%s", host);
            }
        }
        if (sk["port"].is<unsigned int>()) {
            uint16_t port = sk["port"].as<unsigned int>();
            if (changed || !sk["host"].is<const char *>()) {
                p.putUInt("port", port);
                changed = true;
                net::logf("[mgr] applied sk.port=%u", port);
            }
        }
        if (sk["token"].is<const char *>()) {
            const char *tok = sk["token"].as<const char *>();
            p.putString("token", tok);
            changed = true;
            net::logf("[mgr] applied sk.token (len=%u)",
                      (unsigned)strlen(tok));
        }
        p.end();
        // SK reconnect picks up changes on next ws.begin via the
        // existing sk-reconnect path - schedule it.
        if (reset_to_auto) {
            net::dispatchCommand("sk-host auto");
        } else if (changed) {
            net::dispatchCommand("sk-reconnect");
        }
    }

    // ---- 3. source priority / freshness windows ---------------------------
    if (cfg["sources"].is<JsonObject>()) {
        JsonObject src = cfg["sources"].as<JsonObject>();
        if (src["priority"].is<JsonArrayConst>()) {
            JsonArrayConst arr = src["priority"].as<JsonArrayConst>();
            boat::Priority p{};
            // Clear out the default order; whatever we set below wins.
            for (auto &slot : p.order) slot = boat::SourceKind::None;
            uint8_t i = 0;
            bool any_rejected = false;
            for (JsonVariantConst v : arr) {
                if (i >= sizeof(p.order) / sizeof(p.order[0])) break;
                const char *name = v.as<const char *>();
                boat::SourceKind k = sources_check::from_string(name);
                if (k == boat::SourceKind::None && name && *name) {
                    record_error("[mgr] reject sources.priority entry %s "
                                 "(unknown source)", name);
                    any_rejected = true;
                    continue;
                }
                p.order[i++] = k;
            }
            if (any_rejected) {
                ok = false;
            } else if (i > 0) {
                boat::set_priority(p);
                net::logf("[mgr] applied sources.priority (%u entries)",
                          (unsigned)i);
            }
        }
        if (src["timeoutsMs"].is<JsonObject>()) {
            JsonObject t = src["timeoutsMs"].as<JsonObject>();
            boat::Timeouts to = boat::get_timeouts();
            bool any = false;
            if (t["nmea2000"].is<uint32_t>())    { to.nmea2000_ms = t["nmea2000"].as<uint32_t>(); any = true; }
            if (t["nmea0183Wifi"].is<uint32_t>()) { to.nmea_wifi_ms = t["nmea0183Wifi"].as<uint32_t>(); any = true; }
            if (t["nmeaWifi"].is<uint32_t>())     { to.nmea_wifi_ms = t["nmeaWifi"].as<uint32_t>(); any = true; }
            if (t["signalk"].is<uint32_t>())      { to.signalk_ms = t["signalk"].as<uint32_t>(); any = true; }
            if (t["demo"].is<uint32_t>())         { to.demo_ms = t["demo"].as<uint32_t>(); any = true; }
            if (any) {
                boat::set_timeouts(to);
                net::logf("[mgr] applied sources.timeoutsMs (n2k=%lu wifi=%lu "
                          "sk=%lu demo=%lu)",
                          (unsigned long)to.nmea2000_ms,
                          (unsigned long)to.nmea_wifi_ms,
                          (unsigned long)to.signalk_ms,
                          (unsigned long)to.demo_ms);
            }
        }
    }

    // ---- 4. UI (theme/brightness) -----------------------------------------
    if (cfg["ui"].is<JsonObject>()) {
        JsonObject ui = cfg["ui"].as<JsonObject>();
        if (ui["brightness"].is<int>()) {
            int b = ui["brightness"].as<int>();
            if (!ui_config::is_valid_brightness(b)) {
                record_error("[mgr] reject ui.brightness=%d (out of 0..255)", b);
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
            if (ui_config::is_valid_theme(t)) {
                app::Command c;
                c.type = app::CommandType::SetTheme;
                strncpy(c.a, t, sizeof(c.a) - 1);
                app::post(c, 100);
            } else {
                record_error("[mgr] reject ui.theme=%s (unknown)", t);
                ok = false;
            }
        }
    }

    // ---- 5. debug (webAuth as a proxy) ------------------------------------
    if (cfg["webAuth"].is<JsonObject>()) {
        JsonObject web_auth = cfg["webAuth"].as<JsonObject>();
        Preferences p;
        p.begin("web", false);
        bool changed = false;
        if (web_auth["enabled"].is<bool>()) {
            bool enabled = web_auth["enabled"].as<bool>();
            p.putUChar("auth", enabled ? 1 : 0);
            changed = true;
            net::logf("[mgr] applied webAuth.enabled=%d", enabled ? 1 : 0);
        }
        if (web_auth["username"].is<const char *>()) {
            const char *user = web_auth["username"].as<const char *>();
            if (strlen(user) > 0 && strlen(user) <= 31) {
                p.putString("user", user);
                changed = true;
                net::logf("[mgr] applied webAuth.username=%s", user);
            } else {
                record_error("[mgr] reject webAuth.username (invalid length)");
                ok = false;
            }
        }
        if (web_auth["password"].is<const char *>()) {
            const char *pass = web_auth["password"].as<const char *>();
            if (strlen(pass) > 0 && strlen(pass) <= 63) {
                p.putString("pass", pass);
                changed = true;
                net::logf("[mgr] applied webAuth.password (len=%u)",
                          (unsigned)strlen(pass));
            } else {
                record_error("[mgr] reject webAuth.password (invalid length)");
                ok = false;
            }
        }
        p.end();
        if (changed) net::logf("[mgr] web API auth updated");
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
    add_auth_headers(http);
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
        int actual_size = http.getSize();
        if (actual_size == 0 ||
            (actual_size > 0 && want_size && (size_t)actual_size != want_size)) {
            // Length-unknown servers return -1; only reject if known
            // and mismatching.
            if (actual_size > 0 && want_size && (size_t)actual_size != want_size) {
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
                record_error("[mgr-ota] sha mismatch want=%s got=%s",
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
    record_error("[mgr-ota] FAILED job=%s detail=%s code=%d",
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
        if (!ui_config::is_valid_brightness(v)) return "invalid_payload";
        app::Command c;
        c.type = app::CommandType::SetBrightness;
        c.i = v;
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "config.reload") == 0 ||
        strcmp(type, "layout.reload") == 0) {
        // Spec 19 §"Commands": layout.reload is treated as
        // config.reload + re-build managed screens. Screens module
        // is one-shot in MVP, so a full layout swap requires a
        // reboot.
        s_config_fetch_pending = true;
        if (strcmp(type, "layout.reload") == 0) {
            net::logf("[mgr] layout.reload accepted (full apply requires "
                      "reboot in MVP)");
        }
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
    if (strcmp(type, "overlay.show") == 0) {
        // Spec 17 §8: spec lists `message` as the primary payload key;
        // accept `text` as a fallback for plugins that named it that way.
        const char *msg = payload["message"] | "";
        if (!*msg) msg = payload["text"] | "";
        if (!*msg) return "invalid_payload";
        app::Command c;
        c.type = app::CommandType::ShowOverlay;
        strncpy(c.a, msg, sizeof(c.a) - 1);
        c.a[sizeof(c.a) - 1] = 0;
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "overlay.clear") == 0) {
        app::Command c;
        c.type = app::CommandType::ClearOverlay;
        return app::post(c, 100) ? "ok" : "busy";
    }
    if (strcmp(type, "touch.mode") == 0) {
        // Spec 17 §8 touch.mode. Payload: {"mode": "poll"|"irq"}.
        // Switching to "irq" requires the board's TOUCH_INT pin to be
        // wired AND the GT911 worker task to be running; on boards
        // where either is missing main_set_touch_mode returns false
        // and we map that to invalid_payload (caller can retry with
        // "poll" which always works).
        const char *mode = payload["mode"] | "";
        if (!*mode) return "invalid_payload";
        return main_set_touch_mode(mode) ? "ok" : "invalid_payload";
    }
    if (strcmp(type, "log.level") == 0) {
        // Spec 17 §8 log.level. Accepts a string ("trace"|"debug"|
        // "info"|"warn"|"error"|"none") or a numeric ESP_LOG_* enum
        // value (0..5). Applied globally via esp_log_level_set("*").
        // We log a confirmation at INFO so the operator sees the
        // change even when the new level silences themselves.
        const char *lvl_s = payload["level"] | "";
        esp_log_level_t lvl = ESP_LOG_INFO;
        bool ok = true;
        if (*lvl_s) {
            if      (!strcmp(lvl_s, "none"))    lvl = ESP_LOG_NONE;
            else if (!strcmp(lvl_s, "error"))   lvl = ESP_LOG_ERROR;
            else if (!strcmp(lvl_s, "warn"))    lvl = ESP_LOG_WARN;
            else if (!strcmp(lvl_s, "info"))    lvl = ESP_LOG_INFO;
            else if (!strcmp(lvl_s, "debug"))   lvl = ESP_LOG_DEBUG;
            else if (!strcmp(lvl_s, "trace") ||
                     !strcmp(lvl_s, "verbose")) lvl = ESP_LOG_VERBOSE;
            else ok = false;
        } else if (payload["level"].is<int>()) {
            int n = payload["level"].as<int>();
            if (n < ESP_LOG_NONE || n > ESP_LOG_VERBOSE) ok = false;
            else lvl = (esp_log_level_t)n;
        } else {
            ok = false;
        }
        if (!ok) return "invalid_payload";
        esp_log_level_set("*", lvl);
        net::logf("[mgr] log.level -> %d", (int)lvl);
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
    add_auth_headers(http);
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
    add_auth_headers(http);
    int code = http.GET();
    if (code == 200) {
        if (!resp_within_cap(http, MAX_COMMANDS_BYTES, "commands")) {
            http.end();
            return code;
        }
        String resp = http.getString();
        http.end();  // close before any nested HTTP calls (acks)
        JsonDocument r;
        if (deserializeJson(r, resp) != DeserializationError::Ok) return code;
        JsonArray cmds = r["commands"].as<JsonArray>();
        size_t n = cmds.size();
        s_pending_cmd_count = n > 255 ? 255 : (uint8_t)n;
        for (JsonObject cmd : cmds) {
            String cid = cmd["id"] | "";
            const char *type = cmd["type"] | "";
            JsonObject payload = cmd["payload"].as<JsonObject>();
            const char *result = execute_command(type, payload);
            net::logf("[mgr] cmd %s type=%s -> %s",
                      cid.c_str(), type, result);
            lock_state();
            s_last_cmd_id = cid;
            s_last_cmd_type = type;
            s_last_cmd_result = result ? result : "";
            s_last_cmd_ms = millis();
            unlock_state();
            if (cid.length()) ack_command(cid, result);
        }
        // After acking, pending drops to zero from our POV - the plugin
        // will repopulate on the next poll if new commands arrived.
        s_pending_cmd_count = 0;
        return 200;
    }
    http.end();
    if (code < 200 || code >= 300) {
        record_error("[mgr] commands fetch -> %d", code);
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
    add_auth_headers(http);
    int code = http.GET();
    if (code == 200) {
        if (!resp_within_cap(http, MAX_CONFIG_BYTES, "config")) {
            http.end();
            return code;
        }
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
                // RenderPlan is ~12 KB - too big for the worker stack,
                // so we heap-allocate it. PSRAM keeps internal SRAM
                // available for LVGL and the network drivers.
                auto *plan_p = (manager_config::RenderPlan *)heap_caps_calloc(
                    1, sizeof(manager_config::RenderPlan), MALLOC_CAP_SPIRAM);
                if (!plan_p) {
                    record_error("[mgr] render plan alloc failed");
                } else {
                    manager_config::ParseError perr;
                    bool parsed = manager_config::parse(
                        cfg.as<JsonObjectConst>(),
                        g.width_px, g.height_px, *plan_p, perr);
                    if (parsed) {
                        s_render_plan = *plan_p;
                        s_render_plan_valid = true;
                        net::logf("[mgr] render plan: widgets=%u screens=%u "
                                  "variant=%s",
                                  (unsigned)plan_p->widget_count,
                                  (unsigned)plan_p->screen_count,
                                  plan_p->layout_variant[0]
                                      ? plan_p->layout_variant
                                      : "(none)");
                    } else {
                        record_error("[mgr] render plan parse failed: %s at %s",
                                  manager_config::parse_code_to_string(perr.code),
                                  perr.path[0] ? perr.path : "(root)");
                    }
                    if (parsed && plan_p->screen_count > 0) {
                        // Hand the plan off to the LVGL/UI task - all
                        // lv_obj_* calls in manager_screens::apply() must
                        // run there. The pump() consumer frees the blob
                        // after handling.
                        app::Command c;
                        c.type = app::CommandType::ApplyManagedScreens;
                        c.blob = plan_p;
                        c.blob_len = sizeof(manager_config::RenderPlan);
                        if (!app::post(c, 100)) {
                            net::logf("[mgr] ApplyManagedScreens post failed");
                            heap_caps_free(plan_p);
                        }
                        // ownership transferred to the queue on success
                    } else {
                        heap_caps_free(plan_p);
                    }
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
    add_auth_headers(http);
    int code = http.POST(payload);
    if (code == 401 || code == 403) {
        net::logf("[mgr] heartbeat auth failed (%d) - will re-register", code);
        lock_state();
        s_token = "";
        s_auth = AuthState::Unprovisioned;
        s_force_register = true;
        unlock_state();
    } else if (code == 404) {
        // Device id changed (e.g. via network.hostname config) or the
        // plugin record was deleted. Re-register so the plugin learns
        // the new id; keep the token in place since auth itself is fine.
        net::logf("[mgr] heartbeat 404 - re-registering");
        s_force_register = true;
    } else if (code == 200 &&
               resp_within_cap(http, MAX_HEARTBEAT_RESP_BYTES, "heartbeat")) {
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
        record_error("[mgr] heartbeat -> %d", code);
    }
    http.end();
    s_last_heartbeat_ms = millis();
    s_last_heartbeat_code = code;
    // do_register sets s_health on register success/failure, but until
    // now the heartbeat path never wrote it. That left already-
    // provisioned boots reporting health=idle indefinitely even though
    // 200s were flowing in. Mirror the same rule here.
    s_health = (code >= 200 && code < 300) ? HealthState::Heartbeating
                                            : HealthState::Failed;

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
            if (s_sk_token.length()) hc.addHeader("Authorization", String("Bearer ") + s_sk_token); else hc.addHeader("Authorization", String("Bearer ") + s_token);
            if (s_token.length()) hc.addHeader("X-EspDisp-Authorization", String("Bearer ") + s_token);
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
        xTaskCreatePinnedToCore(worker, "mgr", 16384, nullptr, 1, &s_task, 0);
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
    s.has_sk_token = s_sk_token.length() > 0;
    s.last_register_ms = s_last_register_ms;
    s.last_register_code = s_last_register_code;
    s.last_heartbeat_ms = s_last_heartbeat_ms;
    s.last_heartbeat_code = s_last_heartbeat_code;
    s.heartbeat_interval_ms = s_heartbeat_interval_ms;
    s.command_poll_interval_ms = s_command_poll_interval_ms;
    s.device_id = device_identity::get().device_id;
    s.config_version = s_applied_config_version;
    s.config_hash = s_applied_config_hash;
    lock_state();
    s.pending_cmd_count = s_pending_cmd_count;
    s.last_cmd_ms = s_last_cmd_ms;
    s.last_cmd_id = s_last_cmd_id;
    s.last_cmd_type = s_last_cmd_type;
    s.last_cmd_result = s_last_cmd_result;
    s.ota_in_flight = s_ota_in_flight;
    s.ota_confirm_pending = s_ota_confirm_pending;
    s.ota_job_id = s_ota_job_id;
    unlock_state();
    return s;
}

bool handleSerialCommand(const String &line) {
    if (!line.startsWith("manager") && !line.startsWith("mgr-")) {
        return false;
    }

    if (line == "manager-status" || line == "manager") {
        Status st = status();
        net::logf("[mgr] auth=%s health=%s endpoint=%s token=%s sk_token=%s",
                  st.auth == AuthState::Provisioned ? "provisioned" : "unprov",
                  st.health == HealthState::Heartbeating ? "hb"
                    : st.health == HealthState::Registering ? "reg"
                    : st.health == HealthState::Failed ? "failed" : "idle",
                  st.endpoint.length() ? st.endpoint.c_str() : "(none)",
                  st.has_token ? "set" : "none",
                  st.has_sk_token ? "set" : "none");
        net::logf("[mgr] last_register=%dms ago code=%d  "
                  "last_hb=%dms ago code=%d",
                  (int)(millis() - st.last_register_ms),
                  st.last_register_code,
                  (int)(millis() - st.last_heartbeat_ms),
                  st.last_heartbeat_code);
        net::logf("[mgr] cfg ver=%s hash=%s  pending_cmds=%u  "
                  "last_cmd=%s/%s -> %s (%lums ago)",
                  st.config_version.length() ? st.config_version.c_str() : "-",
                  st.config_hash.length() ? st.config_hash.c_str() : "-",
                  (unsigned)st.pending_cmd_count,
                  st.last_cmd_id.length() ? st.last_cmd_id.c_str() : "-",
                  st.last_cmd_type.length() ? st.last_cmd_type.c_str() : "-",
                  st.last_cmd_result.length() ? st.last_cmd_result.c_str() : "-",
                  st.last_cmd_ms ? (unsigned long)(millis() - st.last_cmd_ms) : 0UL);
        net::logf("[mgr] ota in_flight=%d job=%s confirm_pending=%d",
                  (int)st.ota_in_flight,
                  st.ota_job_id.length() ? st.ota_job_id.c_str() : "-",
                  (int)st.ota_confirm_pending);
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
    if (line.startsWith("manager-sk-token ")) {
        String tok = line.substring(17);
        tok.trim();
        if (tok == "clear") {
            s_sk_token = "";
        } else {
            s_sk_token = tok;
        }
        save_prefs();
        net::logf("[mgr] sk_token %s (len=%u)",
                  s_sk_token.length() ? "saved" : "cleared",
                  (unsigned)s_sk_token.length());
        s_force_register = true;
        return true;
    }
    if (line == "manager-forget") {
        lock_state();
        s_endpoint = "";
        s_token = "";
        s_sk_token = "";
        s_auth = AuthState::Unprovisioned;
        save_prefs();
        unlock_state();
        net::logf("[mgr] forgotten");
        return true;
    }
    if (line == "manager-discover") {
        // Spec 18 §4: query the LAN for _espdisp-mgmt._tcp services.
        // The plugin advertises the same service when running on a
        // SignalK host with mDNS enabled. We log every hit; if exactly
        // one is found, auto-set the endpoint and trigger a register.
        if (WiFi.status() != WL_CONNECTED) {
            net::logf("[mgr] discover: wifi not connected");
            return true;
        }
        int n = MDNS.queryService("espdisp-mgmt", "tcp");
        net::logf("[mgr] discover: %d hit(s) for _espdisp-mgmt._tcp", n);
        for (int i = 0; i < n; ++i) {
            String host = MDNS.hostname(i);
            uint16_t port = MDNS.port(i);
            IPAddress ip = MDNS.IP(i);
            net::logf("[mgr]   [%d] %s @ %s:%u",
                      i, host.c_str(), ip.toString().c_str(), (unsigned)port);
        }
        if (n == 1) {
            IPAddress ip = MDNS.IP(0);
            uint16_t port = MDNS.port(0);
            // Prefer the resolved IP - .local hostnames re-resolve at
            // every HTTP call which is wasteful and flaky on iOS hot-
            // spots that filter mDNS.
            String url = String("http://") + ip.toString() + ":" + String(port);
            lock_state();
            s_endpoint = url;
            if (!s_token.length()) s_auth = AuthState::Unprovisioned;
            save_prefs();
            s_force_register = true;
            unlock_state();
            net::logf("[mgr] discover: endpoint set to %s; will register",
                      url.c_str());
        } else if (n > 1) {
            net::logf("[mgr] discover: multiple hits - pick one via "
                      "`manager-register http://<host>:<port>`");
        }
        return true;
    }
    // ---- spec 17 §5 recent errors -------------------------------------
    if (line == "manager-errors") {
        size_t n = error_log::size();
        if (n == 0) {
            net::logf("[mgr] no recent errors");
            return true;
        }
        error_log::Entry buf[error_log::MAX_ENTRIES];
        size_t got = error_log::copy(buf, error_log::MAX_ENTRIES);
        net::logf("[mgr] recent errors (%u, oldest first):", (unsigned)got);
        for (size_t i = 0; i < got; ++i) {
            net::logf("[mgr]   [%u] t=%lums %s",
                      (unsigned)i,
                      (unsigned long)buf[i].timestamp_ms,
                      buf[i].message);
        }
        return true;
    }
    if (line == "manager-errors clear") {
        error_log::clear();
        net::logf("[mgr] errors cleared");
        return true;
    }
    // ---- spec 19 D7 diagnostic commands -------------------------------
    if (line == "manager-layout") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        net::logf("[mgr] layout variant=%s widgets=%u screens=%u "
                  "(%ux%u) hash=%s mgr_screens_built=%u",
                  s_render_plan.layout_variant[0] ? s_render_plan.layout_variant
                                                  : "(none)",
                  (unsigned)s_render_plan.widget_count,
                  (unsigned)s_render_plan.screen_count,
                  (unsigned)s_render_plan.display_width,
                  (unsigned)s_render_plan.display_height,
                  s_applied_config_hash.length() ?
                      s_applied_config_hash.c_str() : "(none)",
                  (unsigned)manager_screens::managed_count());
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
              "manager-token <jwt|clear> | manager-sk-token <jwt|clear> | "
              "manager-forget | manager-discover | manager-errors[ clear] | "
              "manager-layout | "
              "manager-widgets | manager-config-dump | font-dump");
    return true;
}

}  // namespace manager
