#include "manager.h"
#include "build_config.h"

// Defined exactly once for the whole firmware here, so the shared
// PSRAM allocator lives at root namespace scope and links the same
// across manager.cpp + web.cpp + anything else that needs it.
#define PSRAM_JSON_DEFINE_SHARED
#include "psram_json.h"
#undef PSRAM_JSON_DEFINE_SHARED

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
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
#include "autopilot.h"
#include "beeper.h"
#include "board.h"
#include "boat_data.h"
#include "device_identity.h"
#include "font_resolver.h"
#include "error_log.h"
#include "hostname_check.h"
#include "log_level_check.h"
#include "sources_check.h"
#include "storage.h"
#include "ui_config_check.h"
#include "manager_endpoint.h"
#include "manager_config.h"
#include "capabilities.h"
#include "config_persist.h"
#include "manager_screens.h"
#include "manager_url.h"
#include "net.h"
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include "knob_remote.h"
#endif

// Spec 17 §8 touch.mode: the toggle lives in main.cpp because that's
// where the GT911 worker + INT pin are owned. Forward-declared with C
// linkage to match main.cpp's extern "C" block.
extern "C" bool main_set_touch_mode(const char *mode);
extern "C" const char *main_touch_mode();
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
constexpr uint32_t MIN_HEARTBEAT_MS = 30000;
constexpr uint32_t MIN_COMMAND_POLL_MS = 15000;
constexpr uint32_t TRANSPORT_FAILURE_BACKOFF_MS = 60000;
constexpr uint32_t LOW_HEAP_BACKOFF_MS = 60000;
// Tuned against this firmware's observed steady-state on the Sunton
// 4848S040 (internal heap ~10-15 KiB free, largest block ~6 KiB at
// idle once LVGL+WiFi+BLE+SK are all running). The original
// 24 KiB free / 8 KiB largest values guarded too aggressively - the
// manager backed off on every heartbeat because the device just never
// has that much internal heap. Now that the JSON tree lives in PSRAM
// and the HTTP body streams, do_heartbeat itself only needs a few KiB
// of internal heap (HTTPClient instance + transient send/recv buffers).
// 6 KiB free / 3 KiB largest leaves enough headroom for HTTPClient's
// own internal allocations without choking the WiFi stack.
// Typed status for the manager's HTTP-fronted entry points
// (do_register, do_heartbeat, fetch_config, poll_commands). Each call
// returns ManagerCall { status, http_code }. The status is exhaustive
// over the failure modes; http_code is the HTTPClient response code
// when status==Ok or HttpError (the >=200 / <300 split), zero
// otherwise.
//
// History: these functions previously returned bare `int` where
// negative numbers meant pre-flight failures and positive numbers
// meant HTTP status. HTTPClient.POST() itself returns negative codes
// for connection_refused, send_failed, etc., which COLLIDED with our
// own pre-flight codes (-1 was both NotProvisioned and
// HTTPC_ERROR_CONNECTION_REFUSED). On 2026-06-03 I misread a
// `heartbeat -> -1` as WiFi-down when it was actually
// connection-refused. The enum disambiguates statically.
enum class ManagerStatus : uint8_t {
    Ok,              // 2xx response, success
    HttpError,       // got an HTTP response but >=300
    NotProvisioned,  // no endpoint / no token saved
    WifiDown,        // WiFi.status() != WL_CONNECTED
    LowHeap,         // manager_heap_ready() refused (backoff)
    BeginFailed,     // http.begin(url) returned false (bad URL)
    PayloadError,    // post_json couldn't serialize / alloc the body
    SendFailed,      // HTTPClient POST returned a negative transport code
    BodyTooLarge,    // resp_within_cap() rejected the response
    BodyParseError,  // deserialize_http_json() failed
};

struct ManagerCall {
    ManagerStatus status;
    int http_code;  // HTTPClient response code; meaningful for Ok/HttpError

    static ManagerCall ok(int code) { return {ManagerStatus::Ok, code}; }
    static ManagerCall http_error(int code) { return {ManagerStatus::HttpError, code}; }
    static ManagerCall fail(ManagerStatus s, int code = 0) { return {s, code}; }

    bool is_ok() const { return status == ManagerStatus::Ok; }
    // True if this call burned a transport (DNS / TCP / TLS) - distinct
    // from pre-flight refusals like NotProvisioned/LowHeap which mean
    // we never went out on the wire.
    bool burned_transport() const {
        return status == ManagerStatus::SendFailed || status == ManagerStatus::HttpError ||
               status == ManagerStatus::BodyTooLarge || status == ManagerStatus::BodyParseError;
    }
    // True if we declined to go out on the wire at all (not provisioned, low
    // heap, or WiFi down) - the complement of burned_transport for failures.
    bool preflight_refusal() const {
        return status == ManagerStatus::NotProvisioned || status == ManagerStatus::WifiDown ||
               status == ManagerStatus::LowHeap;
    }
};

static const char *to_str(ManagerStatus s) {
    switch (s) {
    case ManagerStatus::Ok:
        return "Ok";
    case ManagerStatus::HttpError:
        return "HttpError";
    case ManagerStatus::NotProvisioned:
        return "NotProvisioned";
    case ManagerStatus::WifiDown:
        return "WifiDown";
    case ManagerStatus::LowHeap:
        return "LowHeap";
    case ManagerStatus::BeginFailed:
        return "BeginFailed";
    case ManagerStatus::PayloadError:
        return "PayloadError";
    case ManagerStatus::SendFailed:
        return "SendFailed";
    case ManagerStatus::BodyTooLarge:
        return "BodyTooLarge";
    case ManagerStatus::BodyParseError:
        return "BodyParseError";
    }
    return "?";
}

constexpr size_t MIN_MANAGER_INTERNAL_HEAP = 6 * 1024;
constexpr size_t MIN_MANAGER_INTERNAL_BLOCK = 3 * 1024;
constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 1000;
constexpr uint16_t HTTP_READ_TIMEOUT_MS = 1500;

// PsramJsonAllocator moved to include/psram_json.h. The shared
// definition lives at file scope (outside both `namespace manager`
// and the anonymous namespace) further down so the symbol resolves
// to `::yeyboats::psram_json` and other TUs (notably web.cpp's
// handle_state) can link against the same instance.

// Local alias to keep the existing call sites short.
auto &s_json_allocator = ::yeyboats::psram_json;

String s_endpoint;
String s_token;     // device/dev/provision token sent as X-YeyBoats-Authorization
String s_sk_token;  // SignalK server bearer token used to pass SK security
manager_endpoint::DiscoveryMethod s_discovery_method = manager_endpoint::DiscoveryMethod::None;
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
// builder. Lazily PSRAM-allocated on first parse — keeping a ~12 KB
// `RenderPlan` in .bss starved internal SRAM (~30 KB documented
// healthy → ~4 KB observed on May-30 build). The parser temporary
// at the apply site already went to PSRAM for the same reason; this
// matches that for the resident copy. All reads gate on
// `s_render_plan_valid` so the nullptr-until-allocated transition is
// already covered.
manager_config::RenderPlan *s_render_plan = nullptr;
bool s_render_plan_valid = false;

static manager_config::RenderPlan *ensure_render_plan() {
    if (!s_render_plan) {
        s_render_plan = (manager_config::RenderPlan *)heap_caps_calloc(
            1, sizeof(manager_config::RenderPlan), MALLOC_CAP_SPIRAM);
    }
    return s_render_plan;
}
String s_desired_config_version = "";
String s_desired_config_hash = "";
volatile bool s_config_fetch_pending = false;
volatile uint32_t s_last_register_ms = 0;
volatile int s_last_register_code = 0;
volatile uint32_t s_last_heartbeat_ms = 0;
volatile int s_last_heartbeat_code = 0;
// S5: labelled heartbeat outcome + classified failure counters. Written under
// s_state_mtx in the heartbeat path; read in status().
String s_last_heartbeat_status = "";
volatile uint32_t s_hb_preflight_refusals = 0;
volatile uint32_t s_hb_transport_failures = 0;
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
// Spec 17 §6 OTA policy. Defaults: pull-OTA enabled, no max size
// (0 == unbounded), sha256 required. These can be tightened by the
// operator via cfg["ota"]. Persisted in NVS under the manager
// namespace so a policy survives reboot.
bool s_ota_enabled = true;
uint32_t s_ota_max_size = 0;
bool s_ota_require_sha = true;
SemaphoreHandle_t s_state_mtx = nullptr;

void lock_state() {
    if (s_state_mtx) xSemaphoreTake(s_state_mtx, portMAX_DELAY);
}
void unlock_state() {
    if (s_state_mtx) xSemaphoreGive(s_state_mtx);
}

void load_prefs() {
    {
        storage::Namespace p(NS, true);
        s_endpoint = String(p.get_string("endpoint", "").c_str());
        s_token = String(p.get_string("token", "").c_str());
        s_sk_token = String(p.get_string("sk_token", "").c_str());
        s_discovery_method =
            manager_endpoint::discovery_method_from_string(p.get_string("disc", "").c_str());
        s_applied_config_version = String(p.get_string("cfg_ver", "v0").c_str());
        s_applied_config_hash = String(p.get_string("cfg_hash", "").c_str());
        s_ota_enabled = p.get_u8("ota_en", 1) != 0;
        s_ota_max_size = p.get_u32("ota_max", 0);
        s_ota_require_sha = p.get_u8("ota_sha", 1) != 0;
    }
    if (s_endpoint.length() && s_discovery_method == manager_endpoint::DiscoveryMethod::None) {
        s_discovery_method = manager_endpoint::DiscoveryMethod::Stored;
    }
    // Rebrand migration: a device provisioned before the plugin rename stored the
    // old plugin base path. Rewrite it to the current slug in-place so config/OTA
    // fetches land on the renamed manager without a manual re-point, and persist
    // the rewrite so it survives the next reboot.
    {
        static const char *kOldBase = "/plugins/espdisp-manager";
        const int at = s_endpoint.indexOf(kOldBase);
        if (at >= 0) {
            s_endpoint = s_endpoint.substring(0, at) + "/plugins/yey-boats-display-manager" +
                         s_endpoint.substring(at + (int)strlen(kOldBase));
            storage::Namespace w(NS, false);
            w.put_string("endpoint", s_endpoint.c_str());
            net::logf("[mgr] migrated stored endpoint to renamed plugin path");
        }
    }
    s_auth = s_token.length() ? AuthState::Provisioned : AuthState::Unprovisioned;
}

void save_prefs() {
    storage::Namespace p(NS, false);
    p.put_string("endpoint", s_endpoint.c_str());
    p.put_string("token", s_token.c_str());
    p.put_string("sk_token", s_sk_token.c_str());
    p.put_string("disc", manager_endpoint::discovery_method_to_string(s_discovery_method));
    p.put_string("cfg_ver", s_applied_config_version.c_str());
    p.put_string("cfg_hash", s_applied_config_hash.c_str());
    p.put_u8("ota_en", s_ota_enabled ? 1 : 0);
    p.put_u32("ota_max", s_ota_max_size);
    p.put_u8("ota_sha", s_ota_require_sha ? 1 : 0);
}

// Hard caps on response bodies we'll accept from the plugin. A hostile
// or buggy server could return arbitrarily large payloads; reading
// everything into a String would OOM the device. These are generous
// for legitimate payloads but bounded.
constexpr int MAX_DISCOVERY_BYTES = 4 * 1024;
constexpr int MAX_HEARTBEAT_RESP_BYTES = 4 * 1024;
constexpr int MAX_CONFIG_BYTES = 32 * 1024;
constexpr int MAX_COMMANDS_BYTES = 8 * 1024;

void record_error(const char *fmt, ...);

struct PsramJsonPayload {
    uint8_t *data = nullptr;
    size_t len = 0;

    PsramJsonPayload() = default;
    ~PsramJsonPayload() {
        if (data) heap_caps_free(data);
    }

    PsramJsonPayload(const PsramJsonPayload &) = delete;
    PsramJsonPayload &operator=(const PsramJsonPayload &) = delete;
};

bool serialize_json_payload(JsonDocument &doc, PsramJsonPayload &payload, const char *who) {
    size_t len = measureJson(doc);
    auto *buf = (uint8_t *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)heap_caps_malloc(len + 1, MALLOC_CAP_8BIT);
    if (!buf) {
        record_error("[mgr] %s payload alloc failed (%u bytes)", who, (unsigned)(len + 1));
        return false;
    }
    size_t written = serializeJson(doc, (char *)buf, len + 1);
    if (written != len) {
        heap_caps_free(buf);
        record_error("[mgr] %s payload serialize failed (%u/%u)", who, (unsigned)written,
                     (unsigned)len);
        return false;
    }
    payload.data = buf;
    payload.len = len;
    return true;
}

// Returns HTTP response code on success, an HTTPClient HTTPC_ERROR_*
// negative on transport failure, or `INT_MIN` for "could not allocate
// or serialize the payload" - chosen to avoid colliding with any
// HTTPClient error value (all of which are -1..-11).
static constexpr int POST_JSON_PAYLOAD_ERROR = INT_MIN;
int post_json(HTTPClient &http, JsonDocument &doc, const char *who) {
    PsramJsonPayload payload;
    if (!serialize_json_payload(doc, payload, who)) return POST_JSON_PAYLOAD_ERROR;
    return http.POST(payload.data, payload.len);
}

bool deserialize_http_json(HTTPClient &http, JsonDocument &doc, const char *who) {
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (err != DeserializationError::Ok) {
        record_error("[mgr] %s JSON parse failed: %s", who, err.c_str());
        return false;
    }
    return true;
}

bool manager_heap_ready(const char *op) {
    size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free < MIN_MANAGER_INTERNAL_HEAP || largest < MIN_MANAGER_INTERNAL_BLOCK) {
        record_error("[mgr] skip %s: low internal heap free=%u largest=%u", op, (unsigned)free,
                     (unsigned)largest);
        return false;
    }
    return true;
}

// Returns true iff the Content-Length header (if any) is within `cap`.
// HTTPClient::getSize() returns -1 when the server omits Content-Length;
// in that case we accept the read but rely on the small timeouts to
// bound it.
bool resp_within_cap(HTTPClient &http, int cap, const char *who) {
    int sz = http.getSize();
    if (sz > cap) {
        net::logf("[mgr] %s response too large (%d > %d) - dropping", who, sz, cap);
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

// Compose Authorization (SK security) + X-YeyBoats-Authorization (plugin auth)
// for any plugin HTTP request. SK security needs a server-issued token in the
// standard Authorization header; the plugin itself reads X-YeyBoats-Authorization
// first (see yey-boats-display-manager index.js authFrom). When the device only
// has one token (e.g. talking to the standalone mock), both headers carry it.
//
// Bootstrap: when s_token is empty (fresh device, no NVS entry) we
// fall back to MANAGER_PROVISION_TOKEN for the X-YeyBoats-Authorization
// slot. The plugin's dev-shared-token mode accepts that as a valid
// provisioning credential on /devices/register and echoes it back as
// the device token. Without this fallback the very first register call
// 401s and the worker churns the heap retrying every 5 s forever.
void add_auth_headers(HTTPClient &http) {
    if (s_sk_token.length()) {
        http.addHeader("Authorization", String("Bearer ") + s_sk_token);
    } else if (s_token.length()) {
        http.addHeader("Authorization", String("Bearer ") + s_token);
    }
    const char *plugin_tok = s_token.length() ? s_token.c_str() : MANAGER_PROVISION_TOKEN;
    if (plugin_tok && *plugin_tok) {
        http.addHeader("X-YeyBoats-Authorization", String("Bearer ") + plugin_tok);
    }
}

void prepare_http(HTTPClient &http, bool json = false) {
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_READ_TIMEOUT_MS);
    http.setReuse(false);
    http.addHeader("Connection", "close");
    if (json) http.addHeader("Content-Type", "application/json");
    add_auth_headers(http);
}

uint32_t clamped_interval(uint32_t value, uint32_t fallback, uint32_t minimum) {
    if (value == 0) return fallback;
    return value < minimum ? minimum : value;
}

void apply_manager_intervals(uint32_t heartbeat_ms, uint32_t command_poll_ms) {
    if (heartbeat_ms) {
        s_heartbeat_interval_ms =
            clamped_interval(heartbeat_ms, DEFAULT_HEARTBEAT_MS, MIN_HEARTBEAT_MS);
    }
    if (command_poll_ms) {
        s_command_poll_interval_ms =
            clamped_interval(command_poll_ms, DEFAULT_COMMAND_POLL_MS, MIN_COMMAND_POLL_MS);
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

// GET /.well-known/yeyboats-management for a given base. Best-effort: if
// the plugin advertises intervals or a basePath override, apply them.
// Returns the HTTP code (or negative on transport error).
int fetch_discovery(const String &base, String *out_base_path = nullptr) {
    HTTPClient http;
    String url = build_url_from_base(base, "/.well-known/yeyboats-management");
    if (!http.begin(url)) return -3;
    prepare_http(http);
    int code = http.GET();
    if (code == 200 && resp_within_cap(http, MAX_DISCOVERY_BYTES, "discovery")) {
        JsonDocument d(&s_json_allocator);
        if (deserialize_http_json(http, d, "discovery")) {
            apply_manager_intervals(d["intervals"]["heartbeatMs"] | 0,
                                    d["intervals"]["commandPollMs"] | 0);
            if (out_base_path && d["basePath"].is<const char *>()) {
                *out_base_path = d["basePath"].as<const char *>();
            }
            net::logf("[mgr] discovery: hb=%ums poll=%ums", (unsigned)s_heartbeat_interval_ms,
                      (unsigned)s_command_poll_interval_ms);
        }
    }
    http.end();
    return code;
}

// POST /devices/register. On 200, store the returned bearer token +
// any heartbeat/command-poll intervals the plugin reports.
ManagerCall do_register() {
    if (s_endpoint.length() == 0) return ManagerCall::fail(ManagerStatus::NotProvisioned);
    if (WiFi.status() != WL_CONNECTED) return ManagerCall::fail(ManagerStatus::WifiDown);
    if (!manager_heap_ready("register")) return ManagerCall::fail(ManagerStatus::LowHeap);
    s_health = HealthState::Registering;

    JsonDocument body(&s_json_allocator);
    device_identity::to_json_doc(body);

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
        prepare_http(http, true);
        code = post_json(http, body, "register");
        if (code == 200 || code == 201) {
            if (resp_within_cap(http, MAX_HEARTBEAT_RESP_BYTES, "register")) {
                successful_base = bases[i];
                JsonDocument r(&s_json_allocator);
                if (deserialize_http_json(http, r, "register")) {
                    if (successful_base.length() && successful_base != s_endpoint) {
                        s_endpoint = successful_base;
                        save_prefs();
                    }
                    const char *tok = r["deviceToken"] | "";
                    if (tok && *tok) {
                        s_token = tok;
                        s_auth = AuthState::Provisioned;
                        save_prefs();
                        net::logf("[mgr] registered ok (token_len=%u)", (unsigned)s_token.length());
                    }
                    uint32_t hb = r["heartbeat"]["intervalMs"] | 0;
                    if (!hb) hb = r["heartbeat_interval_ms"] | 0;
                    uint32_t poll = r["commands"]["pollMs"] | 0;
                    if (!poll) poll = r["command_poll_interval_ms"] | 0;
                    apply_manager_intervals(hb, poll);
                }
            }
            http.end();
            break;
        }
        http.end();
        // 404 from /plugins/... means this is likely the standalone mock.
        if (code != 404) break;
    }
    if (!(code == 200 || code == 201)) {
        record_error("[mgr] register -> %d", code);
    }
    s_last_register_ms = millis();
    s_last_register_code = code;
    s_health = (code >= 200 && code < 300) ? HealthState::Heartbeating : HealthState::Failed;
    // post_json sentinel disambiguation: PAYLOAD_ERROR vs HTTPClient
    // transport negative (both <0, but only one means "we built a bad
    // body"). >=200/<300 = Ok, any other positive = HttpError, any
    // negative = SendFailed (the loop above already records the
    // numeric value via record_error).
    if (code == POST_JSON_PAYLOAD_ERROR) return ManagerCall::fail(ManagerStatus::PayloadError);
    if (code < 0) return ManagerCall::fail(ManagerStatus::SendFailed, code);
    if (code >= 200 && code < 300) return ManagerCall::ok(code);
    return ManagerCall::http_error(code);
}

void build_status_body(JsonDocument &doc) {
    const auto &id = device_identity::get();
    doc["deviceId"] = id.device_id;
    doc["device_id"] = id.device_id;

    board::Geometry g = board::geometry();
    board::Capabilities bc = board::capabilities();
    String theme_name;
    {
        storage::Namespace ui_prefs("ui", true);
        theme_name = String(ui_prefs.get_string("theme", "night").c_str());
    }

    JsonObject net_o = doc["network"].to<JsonObject>();
    net_o["wifi_up"] = net::wifiUp();
    net_o["state"] = net::wifiStateName();
    net_o["ip"] = net::ipString();
    net_o["ssid"] = net::ssidString();
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
    // "live" and "no-data" both mean the WS link is up; only
    // "disconnected"/"stalled" should report connected=false upstream.
    signalk_o["connected"] = (sk_state == "live" || sk_state == "no-data");
    signalk_o["state"] = sk_state;

    JsonObject ui_o = doc["ui"].to<JsonObject>();
    ui_o["uptime_ms"] = millis();
    ui_o["screen"] = ui::current_id();
    // Full switchable-view list so the manager can offer the device's REAL
    // screens instead of a guessed/generated set (the firmware's loaded layout
    // can diverge from the manager-generated config). Non-hidden screens only:
    // hidden utility screens (settings, touch-cal, wifi) are not user switch
    // targets. The doc is ArduinoJson v7 (elastic), so this small array grows
    // on the heap and is freed with the doc after serialization.
    {
        JsonArray screens_a = ui_o["screens"].to<JsonArray>();
        size_t n = ui::screen_count();
        for (size_t i = 0; i < n; ++i) {
            const char *sid = nullptr, *stitle = nullptr;
            bool shidden = false;
            if (!ui::screen_info((int)i, &sid, &stitle, &shidden)) continue;
            if (shidden || !sid) continue;
            JsonObject so = screens_a.add<JsonObject>();
            so["id"] = sid;
            so["title"] = stitle ? stitle : sid;
        }
    }
    ui_o["theme"] = theme_name;
    ui_o["brightness"] = ui::brightness();
    ui_o["layoutVariant"] = s_render_plan_valid ? s_render_plan->layout_variant : "";
    ui_o["widgetVariant"] = s_render_plan_valid ? s_render_plan->widget_variant : "";
    ui_o["widgetConfigHash"] = s_applied_config_hash;
    // Capability manifest: per-view-type limits/attrs the layout editor gates
    // to. Self-describing so the manager needs no per-firmware-version table.
    capabilities::build_manifest(ui_o["capabilities"].to<JsonObject>());

    JsonObject display_o = doc["display"].to<JsonObject>();
    display_o["width"] = g.width_px;
    display_o["height"] = g.height_px;
    display_o["rotation"] = g.rotation;
    display_o["brightness"] = ui::brightness();
    display_o["shape"] = board::shape_name(g.shape);
    display_o["density"] = board::density_class_name(g.density_class);
    display_o["layoutClass"] = board::layout_class_name(g.layout_class);
    JsonObject usable = display_o["usableArea"].to<JsonObject>();
    usable["x"] = g.usable_x;
    usable["y"] = g.usable_y;
    usable["width"] = g.usable_width;
    usable["height"] = g.usable_height;

    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["heap_free_kb"] = (uint32_t)(ESP.getFreeHeap() / 1024);
    mem["internal_free_kb"] =
        (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    mem["internal_largest_block_kb"] =
        (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    mem["internal_min_free_kb"] =
        (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    mem["psram_free_kb"] = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    {
        float tC = board::chipTempC();
        if (!isnan(tC)) mem["chip_temp_c"] = tC;
    }

    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["version"] = id.firmware_version;
    fw["build_time"] = id.build_time;
    fw["git_commit"] = id.git_commit;

    JsonObject touch = doc["touch"].to<JsonObject>();
    touch["mode"] = main_touch_mode();
    touch["controller"] = board::touch_kind_name(bc.touch);
    touch["interrupt"] = bc.touch_interrupt;

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
    n2k_o["hardwareCan"] = bc.nmea2000_can;
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
    // Spec 17 §6 OTA policy snapshot so the plugin can see what the
    // device is enforcing.
    JsonObject otaPolicy = ota["policy"].to<JsonObject>();
    otaPolicy["enabled"] = s_ota_enabled;
    otaPolicy["requireSha256"] = s_ota_require_sha;
    otaPolicy["maxSizeBytes"] = s_ota_max_size;

    JsonObject web_auth = doc["webAuth"].to<JsonObject>();
    {
        storage::Namespace web_prefs("web", true);
        web_auth["enabled"] = web_prefs.get_u8("auth", 0) != 0;
        web_auth["username"] = web_prefs.get_string("user", "espdisp");
        web_auth["passwordSet"] = !web_prefs.get_string("pass", "").empty();
    }

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
            } else if (net::isLegacyDefaultDeviceId(String(hn))) {
                // The plugin's default config carries
                // `network.hostname=espdisp-device` (the literal legacy
                // fallback from secrets.h). A device that has already
                // resolved a MAC-derived id MUST NOT rename itself back
                // to that placeholder - doing so triggered a periodic
                // reboot loop (apply rename -> reboot -> migrate back to
                // MAC id -> config drift -> rename again, every ~60 s).
                // Refuse the rename silently; the manager's discovery
                // path keeps using whatever the device actually reports.
                net::logf_at(net::LOG_DEBUG,
                             "[mgr] ignore network.hostname=%s (placeholder; "
                             "would clobber MAC-derived id %s)",
                             hn, device_identity::get().device_id);
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
        {
            storage::Namespace p("sk", false);
            if (sk["host"].is<const char *>()) {
                const char *host = sk["host"].as<const char *>();
                bool use_mdns = sk["useMdns"] | false;
                bool manager_default = use_mdns && strcmp(host, "signalk.local") == 0;
                if (manager_default) {
                    // The SignalK plugin advertises signalk.local as a service
                    // discovery hint. Persisting it as a manual target disables
                    // the firmware's mDNS discovery path and can overwrite a
                    // working local IP configured after flashing.
                    std::string current_host = p.get_string("host", "");
                    if (current_host == "signalk.local") {
                        p.remove("host");
                        p.remove("port");
                        reset_to_auto = true;
                        net::logf("[mgr] cleared persisted sk.host=%s; use mDNS", host);
                    } else {
                        net::logf("[mgr] ignored default sk.host=%s (useMdns=true)", host);
                    }
                } else {
                    p.put_string("host", host);
                    changed = true;
                    net::logf("[mgr] applied sk.host=%s", host);
                }
            }
            if (sk["port"].is<unsigned int>()) {
                uint16_t port = sk["port"].as<unsigned int>();
                if (changed || !sk["host"].is<const char *>()) {
                    p.put_u32("port", port);
                    changed = true;
                    net::logf("[mgr] applied sk.port=%u", port);
                }
            }
            if (sk["token"].is<const char *>()) {
                const char *tok = sk["token"].as<const char *>();
                p.put_string("token", tok);
                changed = true;
                net::logf("[mgr] applied sk.token (len=%u)", (unsigned)strlen(tok));
            }
        }
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
            for (auto &slot : p.order)
                slot = boat::SourceKind::None;
            uint8_t i = 0;
            bool any_rejected = false;
            for (JsonVariantConst v : arr) {
                if (i >= sizeof(p.order) / sizeof(p.order[0])) break;
                const char *name = v.as<const char *>();
                boat::SourceKind k = sources_check::from_string(name);
                if (k == boat::SourceKind::None && name && *name) {
                    record_error("[mgr] reject sources.priority entry %s "
                                 "(unknown source)",
                                 name);
                    any_rejected = true;
                    continue;
                }
                p.order[i++] = k;
            }
            if (any_rejected) {
                ok = false;
            } else if (i > 0) {
                boat::set_priority(p);
                net::logf("[mgr] applied sources.priority (%u entries)", (unsigned)i);
            }
        }
        if (src["timeoutsMs"].is<JsonObject>()) {
            JsonObject t = src["timeoutsMs"].as<JsonObject>();
            boat::Timeouts to = boat::get_timeouts();
            bool any = false;
            if (t["nmea2000"].is<uint32_t>()) {
                to.nmea2000_ms = t["nmea2000"].as<uint32_t>();
                any = true;
            }
            if (t["nmea0183Wifi"].is<uint32_t>()) {
                to.nmea_wifi_ms = t["nmea0183Wifi"].as<uint32_t>();
                any = true;
            }
            if (t["nmeaWifi"].is<uint32_t>()) {
                to.nmea_wifi_ms = t["nmeaWifi"].as<uint32_t>();
                any = true;
            }
            if (t["signalk"].is<uint32_t>()) {
                to.signalk_ms = t["signalk"].as<uint32_t>();
                any = true;
            }
            if (t["demo"].is<uint32_t>()) {
                to.demo_ms = t["demo"].as<uint32_t>();
                any = true;
            }
            if (any) {
                boat::set_timeouts(to);
                net::logf("[mgr] applied sources.timeoutsMs (n2k=%lu wifi=%lu "
                          "sk=%lu demo=%lu)",
                          (unsigned long)to.nmea2000_ms, (unsigned long)to.nmea_wifi_ms,
                          (unsigned long)to.signalk_ms, (unsigned long)to.demo_ms);
            }
        }
    }

    // ---- 3b. NMEA0183-over-WiFi -------------------------------------------
    // Spec 17 §6 "NMEA 0183 WiFi" config section. Routes through the
    // existing nmea-wifi CLI verbs so the apply path stays in sync
    // with operator commands and NVS persistence.
    if (cfg["nmea0183Wifi"].is<JsonObject>()) {
        JsonObject nw = cfg["nmea0183Wifi"].as<JsonObject>();
        const char *mode = nw["mode"] | "";
        const char *host = nw["host"] | "";
        uint32_t port = nw["port"] | 0;

        if (strcmp(mode, "tcp") == 0) {
            // tcp also flips the worker to enabled (see CLI handler).
            // Host is required; empty would be silently parsed as
            // "" by the CLI which never resolves.
            if (!host || !*host) {
                record_error("[mgr] reject nmea0183Wifi.mode=tcp (host missing)");
                ok = false;
            } else {
                String cmd = "nmea-wifi tcp ";
                cmd += host;
                if (port) {
                    cmd += " ";
                    cmd += String(port);
                }
                net::dispatchCommand(cmd);
            }
        } else if (strcmp(mode, "udp") == 0) {
            String cmd = "nmea-wifi udp";
            if (port) {
                cmd += " ";
                cmd += String(port);
            }
            net::dispatchCommand(cmd);
        } else if (*mode) {
            record_error("[mgr] reject nmea0183Wifi.mode=%s (want tcp/udp)", mode);
            ok = false;
        }

        if (nw["enabled"].is<bool>()) {
            bool en = nw["enabled"].as<bool>();
            // tcp/udp blocks already set enabled=true. Only dispatch
            // disable explicitly; "enabled: true" is the no-op path.
            if (!en) net::dispatchCommand("nmea-wifi disable");
        }
    }

    // ---- 3c. NMEA2000 (CAN listen-only) -----------------------------------
    // Spec 17 §6 "NMEA 2000" config section. Routed via the existing
    // n2k CLI so the apply path matches operator commands. The board's
    // capability flag (board::capabilities().nmea2000_can) decides
    // whether enabling makes sense; if the board has no transceiver we
    // accept the config but warn so the operator can spot the mismatch.
    if (cfg["nmea2000"].is<JsonObject>()) {
        JsonObject n2 = cfg["nmea2000"].as<JsonObject>();
        if (n2["pins"].is<JsonObject>()) {
            JsonObject p = n2["pins"].as<JsonObject>();
            int rx = p["rx"] | -1;
            int tx = p["tx"] | -1;
            if (rx >= 0 && tx >= 0) {
                String cmd = "n2k pins ";
                cmd += String(rx);
                cmd += " ";
                cmd += String(tx);
                net::dispatchCommand(cmd);
            }
        }
        if (n2["sniff"].is<bool>()) {
            net::dispatchCommand(n2["sniff"].as<bool>() ? "n2k sniff on" : "n2k sniff off");
        }
        if (n2["txEnabled"].is<bool>()) {
            net::dispatchCommand(n2["txEnabled"].as<bool>() ? "n2k tx on" : "n2k tx off");
        }
        if (n2["enabled"].is<bool>()) {
            bool want_en = n2["enabled"].as<bool>();
            if (want_en && !board::capabilities().nmea2000_can) {
                record_error("[mgr] nmea2000.enabled=true but board has no "
                             "CAN transceiver (n2k will start listening but "
                             "see no frames)");
                // Don't fail the apply - the operator may be wiring an
                // external transceiver and wants the worker up regardless.
            }
            net::dispatchCommand(want_en ? "n2k enable" : "n2k disable");
        }
    }

    // ---- 3d. Autopilot permissions ----------------------------------------
    // Spec 17 §6 "autopilot permissions". The plugin / operator can
    // lock out dangerous actions remotely (engage, heading adjust).
    // Backend selection is included here too because it lives next
    // to the permissions in the plugin's default profile.
    if (cfg["autopilot"].is<JsonObject>()) {
        JsonObject ap = cfg["autopilot"].as<JsonObject>();
        autopilot::Permissions p = autopilot::get_permissions();
        if (ap["allowEngage"].is<bool>()) p.allow_engage = ap["allowEngage"];
        if (ap["allowStandby"].is<bool>()) p.allow_standby = ap["allowStandby"];
        if (ap["allowHeadingAdjust"].is<bool>()) p.allow_heading_adjust = ap["allowHeadingAdjust"];
        autopilot::set_permissions(p);

        if (ap["backend"].is<const char *>()) {
            const char *b = ap["backend"].as<const char *>();
            if (strcmp(b, "signalk") == 0) {
                autopilot::set_default_backend(autopilot::Backend::SignalK);
            } else if (strcmp(b, "nmea2000") == 0) {
                autopilot::set_default_backend(autopilot::Backend::NMEA2000Raymarine);
            } else {
                record_error("[mgr] reject autopilot.backend=%s (want signalk/nmea2000)", b);
                ok = false;
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

    // ---- 4b. OTA policy ---------------------------------------------------
    // Spec 17 §6 "OTA metadata". Lock the policy via the config so an
    // operator can disable updates remotely or require sha256 on every
    // job. Defaults remain permissive (enabled, sha required, no max).
    if (cfg["ota"].is<JsonObject>()) {
        JsonObject ota = cfg["ota"].as<JsonObject>();
        bool changed = false;
        if (ota["enabled"].is<bool>()) {
            s_ota_enabled = ota["enabled"];
            changed = true;
        }
        if (ota["requireSha256"].is<bool>()) {
            s_ota_require_sha = ota["requireSha256"];
            changed = true;
        }
        if (ota["maxSizeBytes"].is<uint32_t>()) {
            s_ota_max_size = ota["maxSizeBytes"].as<uint32_t>();
            changed = true;
        }
        if (changed) {
            save_prefs();
            net::logf("[mgr] applied ota policy: enabled=%d "
                      "require_sha=%d max=%u",
                      (int)s_ota_enabled, (int)s_ota_require_sha, (unsigned)s_ota_max_size);
        }
        // Manager-pushed OTA password (server-sourced provisioning, Slice 7).
        // Persist + apply via net so OTA uses it; never log the value.
        const char *ota_pw = ota["password"] | "";
        if (ota_pw && ota_pw[0]) {
            net::setOtaPassword(ota_pw);
        }
    }

    // ---- 5. debug ---------------------------------------------------------
    // Spec 17 §6 "debug" config section. logLevel accepts the same
    // string ("trace"|"verbose"|"debug"|"info"|"warn"|"error"|"none")
    // OR numeric (0..5) shapes as the spec 17 §8 log.level command,
    // via the shared log_level_check parser.
    if (cfg["debug"].is<JsonObject>()) {
        JsonObject dbg = cfg["debug"].as<JsonObject>();
        if (dbg["logLevel"].is<const char *>()) {
            const char *s = dbg["logLevel"].as<const char *>();
            int n = -1;
            if (log_level_check::from_string(s, &n)) {
                esp_log_level_set("*", (esp_log_level_t)n);
                net::logf("[mgr] applied debug.logLevel=%s (%d)", s, n);
            } else {
                record_error("[mgr] reject debug.logLevel=%s (unknown)", s);
                ok = false;
            }
        } else if (dbg["logLevel"].is<int>()) {
            int n = dbg["logLevel"].as<int>();
            if (log_level_check::is_valid_int(n)) {
                esp_log_level_set("*", (esp_log_level_t)n);
                net::logf("[mgr] applied debug.logLevel=%d", n);
            } else {
                record_error("[mgr] reject debug.logLevel=%d (out of 0..5)", n);
                ok = false;
            }
        }
    }

    if (cfg["webAuth"].is<JsonObject>()) {
        JsonObject web_auth = cfg["webAuth"].as<JsonObject>();
        bool changed = false;
        {
            storage::Namespace p("web", false);
            if (web_auth["enabled"].is<bool>()) {
                bool enabled = web_auth["enabled"].as<bool>();
                p.put_u8("auth", enabled ? 1 : 0);
                changed = true;
                net::logf("[mgr] applied webAuth.enabled=%d", enabled ? 1 : 0);
            }
            if (web_auth["username"].is<const char *>()) {
                const char *user = web_auth["username"].as<const char *>();
                if (strlen(user) > 0 && strlen(user) <= 31) {
                    p.put_string("user", user);
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
                    p.put_string("pass", pass);
                    changed = true;
                    net::logf("[mgr] applied webAuth.password (len=%u)", (unsigned)strlen(pass));
                } else {
                    record_error("[mgr] reject webAuth.password (invalid length)");
                    ok = false;
                }
            }
        }
        if (changed) {
            net::logf("[mgr] web API auth updated");
            net::requestMdnsAdvertise();
        }
    }

    return ok;
}

// F6 - post a state line to the OTA job progress endpoint. No-op if
// no endpoint/token (we still try to install locally, but progress
// is informational).
void post_ota_progress(const String &job_id, const char *state, int progress_pct = -1,
                       const char *detail = nullptr) {
    if (!is_provisioned() || job_id.length() == 0) return;
    if (!manager_heap_ready("ota progress")) return;
    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id + "/firmware/jobs/" +
                 job_id + "/progress";
    if (!http.begin(url)) return;
    prepare_http(http, true);
    JsonDocument body(&s_json_allocator);
    body["state"] = state;
    if (progress_pct >= 0) body["progress_pct"] = progress_pct;
    if (detail) body["detail"] = detail;
    int code = post_json(http, body, "ota progress");
    if (code != 200) {
        net::logf("[mgr-ota] progress %s -> %d", state, code);
    }
    http.end();
}

// Convert a 32-byte SHA-256 digest into 64-char lowercase hex.
void sha256_to_hex(const uint8_t *digest, char out[65]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[64] = 0;
}

void ota_task(void *) {
    esp_task_wdt_delete(NULL);
    s_ota_in_flight = true;
    // Quiesce the LVGL pump for the whole OTA: the sustained 2 MB Update.write
    // erases/writes flash, and the UI task running flash-resident code +
    // churning the PSRAM framebuffer in that window intermittently hangs the
    // device (precompiled arduino-esp32 flash-cache hazard). Resumed only on
    // failure below; the success path reboots.
    app_pause_ui(true);
    String job_id = s_ota_job_id;
    String url = s_ota_url;
    // A manager-relative URL ("/firmware/download/<job>") means pull the binary
    // from the manager over plain HTTP (it proxies the GitHub asset host-side),
    // avoiding device-side HTTPS. Resolve it against the plugin base.
    if (url.startsWith("/")) url = build_url(url.c_str());
    String want_sha = s_ota_sha256;
    size_t want_size = s_ota_size;
    const char *failure_detail = nullptr;
    int outcome_code = -1;

    net::logf("[mgr-ota] start job=%s url=%s size=%u", job_id.c_str(), url.c_str(),
              (unsigned)want_size);
    post_ota_progress(job_id, "accepted", 0);

    HTTPClient http;
    if (!http.begin(url)) {
        failure_detail = "http begin failed";
        goto fail;
    }
    // Send the device token so the manager's download route authorizes this
    // job's device (same auth the device uses for /commands).
    prepare_http(http);
    http.setConnectTimeout(5000);
    http.setTimeout(15000);  // longer for big binaries
    // GitHub release-asset URLs 302-redirect to objects.githubusercontent.com;
    // a SignalK-manager catalog points the job at that public URL. Follow the
    // redirect chain (and re-resolve to the signed asset host) instead of
    // failing on the 302 ("GET non-200"). STRICT follows GET/HEAD only.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
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
                record_error("[mgr-ota] sha mismatch want=%s got=%s", w.c_str(), got_sha);
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
    record_error("[mgr-ota] FAILED job=%s detail=%s code=%d", job_id.c_str(),
                 failure_detail ? failure_detail : "?", outcome_code);
    post_ota_progress(job_id, "failed", -1, failure_detail ? failure_detail : "unknown");
    s_ota_in_flight = false;
    app_pause_ui(false);  // OTA aborted; bring the UI back
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
    if (strcmp(type, "config.reload") == 0 || strcmp(type, "layout.reload") == 0) {
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
        if (!s_ota_enabled) {
            record_error("[mgr-ota] policy: OTA disabled, refusing job");
            return "forbidden";
        }
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
        if (s_ota_require_sha && !*sha) {
            record_error("[mgr-ota] policy: sha256 required, refusing job");
            return "invalid_payload";
        }
        if (s_ota_max_size && size > s_ota_max_size) {
            record_error("[mgr-ota] policy: size %u > max %u, refusing job", (unsigned)size,
                         (unsigned)s_ota_max_size);
            return "invalid_payload";
        }
        s_ota_job_id = job_id;
        s_ota_url = url;
        s_ota_sha256 = sha;
        s_ota_version = ver;
        s_ota_size = size;
        if (xTaskCreatePinnedToCore(ota_task, "mgr-ota", 8192, nullptr, 1, &s_ota_task, 0) !=
            pdPASS) {
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
        // Spec 17 §8 log.level. Payload parser lives in
        // log_level_check::* so the spec 17 §6 cfg["debug"] path can
        // share the same rules. Always logs a confirmation at INFO so
        // the operator sees the change even when the new level would
        // silence the confirmation.
        const char *lvl_s = payload["level"] | "";
        int n = -1;
        if (*lvl_s) {
            if (!log_level_check::from_string(lvl_s, &n)) return "invalid_payload";
        } else if (payload["level"].is<int>()) {
            n = payload["level"].as<int>();
            if (!log_level_check::is_valid_int(n)) return "invalid_payload";
        } else {
            return "invalid_payload";
        }
        esp_log_level_set("*", (esp_log_level_t)n);
        net::logf("[mgr] log.level -> %d", n);
        return "ok";
    }
    return "unsupported_command";
}

void ack_command(const String &cmd_id, const char *result) {
    if (!manager_heap_ready("command ack")) return;
    HTTPClient http;
    String url =
        build_url("/devices/") + device_identity::get().device_id + "/commands/" + cmd_id + "/ack";
    if (!http.begin(url)) return;
    prepare_http(http, true);
    JsonDocument body(&s_json_allocator);
    body["result"] = result;
    int code = post_json(http, body, "command ack");
    if (code != 200) {
        net::logf("[mgr] ack %s -> %d", cmd_id.c_str(), code);
    }
    http.end();
}

ManagerCall poll_commands() {
    if (!is_provisioned()) return ManagerCall::fail(ManagerStatus::NotProvisioned);
    if (WiFi.status() != WL_CONNECTED) return ManagerCall::fail(ManagerStatus::WifiDown);
    if (!manager_heap_ready("commands")) return ManagerCall::fail(ManagerStatus::LowHeap);

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id + "/commands";
    if (!http.begin(url)) return ManagerCall::fail(ManagerStatus::BeginFailed);
    prepare_http(http);
    int code = http.GET();
    if (code == 200) {
        if (!resp_within_cap(http, MAX_COMMANDS_BYTES, "commands")) {
            http.end();
            return ManagerCall::fail(ManagerStatus::BodyTooLarge, code);
        }
        JsonDocument r(&s_json_allocator);
        if (!deserialize_http_json(http, r, "commands")) {
            http.end();
            return ManagerCall::fail(ManagerStatus::BodyParseError, code);
        }
        http.end();  // close before any nested HTTP calls (acks)
        JsonArray cmds = r["commands"].as<JsonArray>();
        size_t n = cmds.size();
        s_pending_cmd_count = n > 255 ? 255 : (uint8_t)n;
        for (JsonObject cmd : cmds) {
            String cid = cmd["id"] | "";
            const char *type = cmd["type"] | "";
            JsonObject payload = cmd["payload"].as<JsonObject>();
            const char *result = execute_command(type, payload);
            net::logf("[mgr] cmd %s type=%s -> %s", cid.c_str(), type, result);
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
        return ManagerCall::ok(code);
    }
    http.end();
    if (code < 0) {
        record_error("[mgr] commands send failed code=%d", code);
        return ManagerCall::fail(ManagerStatus::SendFailed, code);
    }
    if (code < 200 || code >= 300) {
        record_error("[mgr] commands fetch -> %d", code);
        return ManagerCall::http_error(code);
    }
    return ManagerCall::ok(code);
}

// ---- Slice 4: flash-persistent applied config -------------------------
// The raw applied config JSON is stored as a single LittleFS file on the
// 4 MB `spiffs` data partition (NVS single-blob limits ~4 KB; the config
// can be up to MAX_CONFIG_BYTES = 32 KB). The hash + schema version live
// alongside in the "mgr" NVS namespace (cfg_hash already persisted; we add
// cfg_schema), so a boot can decide whether to apply the blob and what
// hash to report without parsing it first. Boot flow:
//   read blob -> should_apply_on_boot() -> deserialize (PSRAM doc) ->
//   apply_config + build managed screens -> report persisted hash.
// On corrupt/stale blob we clear it and fall back to built-in defaults.
constexpr const char *CONFIG_BLOB_PATH = "/applied_config.json";
bool s_littlefs_ready = false;

bool ensure_littlefs() {
    if (s_littlefs_ready) return true;
    // format-on-fail so a fresh / corrupt partition self-heals instead of
    // wedging the persist path forever.
    if (LittleFS.begin(true)) {
        s_littlefs_ready = true;
    } else {
        record_error("[mgr] LittleFS mount failed; config persist disabled");
    }
    return s_littlefs_ready;
}

void clear_persisted_config() {
    if (ensure_littlefs() && LittleFS.exists(CONFIG_BLOB_PATH)) {
        LittleFS.remove(CONFIG_BLOB_PATH);
    }
    storage::Namespace p(NS, false);
    p.remove("cfg_schema");
}

// Write `data[0..len)` (the serialized applied config JSON) to flash, then
// stamp the schema version in NVS. Called from the worker task only.
// Returns true on success. On any failure the blob is removed so a partial
// write can't be mis-read on boot.
bool write_persisted_config(const uint8_t *data, size_t len) {
    if (!ensure_littlefs()) return false;
    File f = LittleFS.open(CONFIG_BLOB_PATH, "w");
    if (!f) {
        record_error("[mgr] persist: open for write failed");
        return false;
    }
    size_t wrote = f.write(data, len);
    f.close();
    if (wrote != len) {
        record_error("[mgr] persist: short write %u/%u", (unsigned)wrote, (unsigned)len);
        LittleFS.remove(CONFIG_BLOB_PATH);
        return false;
    }
    storage::Namespace p(NS, false);
    p.put_u8("cfg_schema", config_persist::SCHEMA_VERSION);
    net::logf("[mgr] persisted applied config (%u bytes)", (unsigned)len);
    return true;
}

// Read the persisted config blob into a PSRAM buffer. Returns the buffer
// (caller heap_caps_free's it) and sets *out_len, or nullptr if absent /
// unreadable / over MAX_CONFIG_BYTES. Never a large stack array.
uint8_t *read_persisted_config(size_t *out_len) {
    *out_len = 0;
    if (!ensure_littlefs() || !LittleFS.exists(CONFIG_BLOB_PATH)) return nullptr;
    File f = LittleFS.open(CONFIG_BLOB_PATH, "r");
    if (!f) return nullptr;
    size_t sz = f.size();
    if (sz == 0 || sz > (size_t)MAX_CONFIG_BYTES) {
        f.close();
        record_error("[mgr] persisted config bad size %u; ignoring", (unsigned)sz);
        return nullptr;
    }
    auto *buf = (uint8_t *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)heap_caps_malloc(sz + 1, MALLOC_CAP_8BIT);
    if (!buf) {
        f.close();
        record_error("[mgr] persisted config alloc failed (%u bytes)", (unsigned)(sz + 1));
        return nullptr;
    }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) {
        heap_caps_free(buf);
        record_error("[mgr] persisted config short read %u/%u", (unsigned)got, (unsigned)sz);
        return nullptr;
    }
    buf[sz] = 0;
    *out_len = sz;
    return buf;
}

// Shared apply path used by both the HTTP fetch and the boot-from-flash
// load. Takes a deserialized `config` object, validates + applies it,
// builds the managed screens, and (on success) updates the resident hash /
// version. The flash persist itself is handled by the fetch caller (which
// owns the raw bytes); boot-load passes persist=false.
//   cfg            : the config object (already a copy in a PSRAM doc)
//   new_version    : version label for this config
//   new_hash       : hash for this config (drift key + reported hash)
// Returns true iff apply_config honored every field.
bool apply_config_object(JsonDocument &cfg, const String &new_version, const String &new_hash) {
    board::Geometry g = board::geometry();
    // RenderPlan is ~12 KB - heap/PSRAM, never the worker stack.
    auto *plan_p = (manager_config::RenderPlan *)heap_caps_calloc(
        1, sizeof(manager_config::RenderPlan), MALLOC_CAP_SPIRAM);
    if (!plan_p) {
        record_error("[mgr] render plan alloc failed");
    } else {
        manager_config::ParseError perr;
        bool parsed = manager_config::parse(cfg.as<JsonObjectConst>(), g.width_px, g.height_px,
                                            *plan_p, perr);
        if (parsed) {
            if (ensure_render_plan()) {
                *s_render_plan = *plan_p;
                s_render_plan_valid = true;
            } else {
                record_error("[mgr] resident render plan alloc failed");
            }
            net::logf("[mgr] render plan: widgets=%u screens=%u variant=%s",
                      (unsigned)plan_p->widget_count, (unsigned)plan_p->screen_count,
                      plan_p->layout_variant[0] ? plan_p->layout_variant : "(none)");
        } else {
            record_error("[mgr] render plan parse failed: %s at %s",
                         manager_config::parse_code_to_string(perr.code),
                         perr.path[0] ? perr.path : "(root)");
        }
        if (parsed && plan_p->screen_count > 0) {
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
        net::requestMdnsAdvertise();
    }
    return ok;
}

// Boot-time restore: load the last applied config from flash and apply it
// so the device renders its configured layout offline, before any manager
// contact. Runs on the manager worker task (LittleFS + apply both want a
// real task stack, not loopTask). Best-effort: a missing / stale-schema /
// corrupt blob is ignored (and cleared) and the device keeps its built-in
// defaults. Sets the resident hash so the first heartbeat reports the
// persisted hash and an unchanged config does NOT trigger a re-fetch.
void load_persisted_config() {
    uint8_t blob_schema = 0;
    {
        storage::Namespace p(NS, true);
        blob_schema = p.get_u8("cfg_schema", 0);
    }
    size_t len = 0;
    uint8_t *raw = read_persisted_config(&len);
    bool has_blob = (raw != nullptr);
    if (!config_persist::should_apply_on_boot(has_blob, blob_schema,
                                              config_persist::SCHEMA_VERSION)) {
        if (has_blob) {
            // Blob present but wrong schema - drop it so we don't keep
            // mis-parsing it every boot.
            heap_caps_free(raw);
            net::logf("[mgr] persisted config schema %u != %u; ignoring + clearing", blob_schema,
                      config_persist::SCHEMA_VERSION);
            clear_persisted_config();
        }
        return;
    }

    JsonDocument doc(&s_json_allocator);
    DeserializationError err = deserializeJson(doc, raw, len);
    if (err != DeserializationError::Ok) {
        heap_caps_free(raw);
        record_error("[mgr] persisted config parse failed: %s; clearing", err.c_str());
        clear_persisted_config();
        return;
    }
    heap_caps_free(raw);

    // The blob is the `config` object itself (we persist the inner object,
    // not the HTTP envelope). version/hash come from NVS (cfg_ver/cfg_hash,
    // already loaded into s_applied_* by load_prefs()).
    String ver = s_applied_config_version;
    String hash = s_applied_config_hash;
    JsonDocument cfg(&s_json_allocator);
    cfg.set(doc.as<JsonObjectConst>());
    bool ok = apply_config_object(cfg, ver, hash);
    if (ok) {
        net::logf("[mgr] restored persisted config from flash: version=%s hash=%s", ver.c_str(),
                  hash.c_str());
    } else {
        // A persisted config that no longer applies cleanly (e.g. a field
        // this firmware dropped) - keep what applied, but don't brick. The
        // resident hash is unchanged so the manager will re-push on drift.
        net::logf("[mgr] persisted config applied partial subset on boot");
    }
}

ManagerCall fetch_config() {
    if (!is_provisioned()) return ManagerCall::fail(ManagerStatus::NotProvisioned);
    if (WiFi.status() != WL_CONNECTED) return ManagerCall::fail(ManagerStatus::WifiDown);
    if (!manager_heap_ready("config fetch")) return ManagerCall::fail(ManagerStatus::LowHeap);

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id + "/config";
    if (!http.begin(url)) return ManagerCall::fail(ManagerStatus::BeginFailed);
    prepare_http(http);
    int code = http.GET();
    if (code == 200) {
        if (!resp_within_cap(http, MAX_CONFIG_BYTES, "config")) {
            http.end();
            return ManagerCall::fail(ManagerStatus::BodyTooLarge, code);
        }
        JsonDocument r(&s_json_allocator);
        if (deserialize_http_json(http, r, "config")) {
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
                JsonDocument cfg(&s_json_allocator);
                cfg.set(cfg_src);
                // Snapshot the prior hash BEFORE apply so the flash-wear
                // gate compares against what is actually on flash, not the
                // value we are about to overwrite in s_applied_config_hash.
                String prev_hash = s_applied_config_hash;
                // Spec 19 D2 -> D5 + Slice 4: build the RenderPlan, apply
                // the config, and (on success) update the resident hash /
                // version - all via the shared helper that boot-from-flash
                // reuses. Failures don't block the legacy ui/sk/network
                // apply; those are honored inside apply_config.
                bool ok = apply_config_object(cfg, new_version, new_hash);
                if (ok) {
                    net::logf("[mgr] config applied: version=%s hash=%s", new_version.c_str(),
                              new_hash.c_str());
                    // Slice 4: persist the applied config to flash, but only
                    // when the hash actually changed (flash-wear rule). We
                    // store the inner `config` object so the boot loader can
                    // re-apply it through the same path.
                    if (config_persist::should_persist(ok, std::string(new_hash.c_str()),
                                                       std::string(prev_hash.c_str()))) {
                        PsramJsonPayload blob;
                        if (serialize_json_payload(cfg, blob, "config persist")) {
                            write_persisted_config(blob.data, blob.len);
                        }
                    } else {
                        net::logf_at(net::LOG_DEBUG, "[mgr] config hash unchanged; "
                                                     "skip flash persist");
                    }
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
    if (code < 0) return ManagerCall::fail(ManagerStatus::SendFailed, code);
    if (code >= 200 && code < 300) return ManagerCall::ok(code);
    return ManagerCall::http_error(code);
}

ManagerCall do_heartbeat() {
    if (!is_provisioned()) return ManagerCall::fail(ManagerStatus::NotProvisioned);
    if (WiFi.status() != WL_CONNECTED) return ManagerCall::fail(ManagerStatus::WifiDown);
    if (!manager_heap_ready("heartbeat")) return ManagerCall::fail(ManagerStatus::LowHeap);

    JsonDocument body(&s_json_allocator);
    build_status_body(body);

    HTTPClient http;
    String url = build_url("/devices/") + device_identity::get().device_id + "/status";
    if (!http.begin(url)) return ManagerCall::fail(ManagerStatus::BeginFailed);
    prepare_http(http, true);
    int code = post_json(http, body, "heartbeat");
    if (code == POST_JSON_PAYLOAD_ERROR) {
        http.end();
        return ManagerCall::fail(ManagerStatus::PayloadError);
    }
    if (code < 0) {
        // HTTPClient transport error - the negative is one of
        // HTTPC_ERROR_*. Record numeric value alongside the status.
        record_error("[mgr] heartbeat send failed code=%d", code);
        http.end();
        return ManagerCall::fail(ManagerStatus::SendFailed, code);
    }
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
    } else if (code == 200 && resp_within_cap(http, MAX_HEARTBEAT_RESP_BYTES, "heartbeat")) {
        // F3: check if the server wants a config update.
        JsonDocument r(&s_json_allocator);
        if (deserialize_http_json(http, r, "heartbeat")) {
            String want_ver = r["desiredConfig"]["version"] | "";
            String want_hash = r["desiredConfig"]["hash"] | "";
            if (!want_ver.length()) want_ver = r["desired_config_version"] | "";
            if (!want_hash.length()) want_hash = r["desired_config_hash"] | "";
            lock_state();
            s_desired_config_version = want_ver;
            s_desired_config_hash = want_hash;
            // Slice 4: the drift rule is pinned in config_persist so the
            // persisted-hash-after-boot path keeps it honest - an unchanged
            // config restored from flash must NOT trigger a re-fetch.
            bool drift = config_persist::should_refetch(
                std::string(want_hash.c_str()), std::string(s_applied_config_hash.c_str()),
                std::string(want_ver.c_str()), std::string(s_applied_config_version.c_str()));
            // Spec 17 §5 response handling: the plugin can ask the
            // device to re-register (e.g. after admin reset or token
            // rotation). Accept both camelCase and snake_case keys.
            bool reregister =
                (r["requestedReregister"] | false) || (r["requested_reregister"] | false);
            unlock_state();
            if (drift) {
                net::logf("[mgr] config drift: have=%s want=%s -> fetching",
                          s_applied_config_version.c_str(), want_ver.c_str());
                s_config_fetch_pending = true;
            }
            if (reregister) {
                net::logf("[mgr] plugin requested re-register");
                s_force_register = true;
            }
        }
    } else if (code < 200 || code >= 300) {
        record_error("[mgr] heartbeat -> %d", code);
    }
    http.end();
    s_last_heartbeat_ms = millis();
    s_last_heartbeat_code = code;
    bool ok = (code >= 200 && code < 300);
    // do_register sets s_health on register success/failure, but until
    // now the heartbeat path never wrote it. That left already-
    // provisioned boots reporting health=idle indefinitely even though
    // 200s were flowing in. Mirror the same rule here.
    s_health = (code >= 200 && code < 300) ? HealthState::Heartbeating : HealthState::Failed;

    // Spec 17 §10 / 18 §11: after a post-OTA boot, the first successful
    // heartbeat triggers a /firmware/confirm POST so the plugin can
    // mark its OTA job complete. We don't have the original job id at
    // boot, so the body just carries the new build's identity - the
    // plugin correlates by deviceId + version+hash.
    if (code >= 200 && code < 300 && s_ota_confirm_pending) {
        HTTPClient hc;
        String confirm_url =
            build_url("/devices/") + device_identity::get().device_id + "/firmware/confirm";
        if (hc.begin(confirm_url)) {
            prepare_http(hc, true);
            JsonDocument cbody(&s_json_allocator);
            const auto &id = device_identity::get();
            cbody["version"] = id.firmware_version;
            cbody["build_time"] = id.build_time;
            cbody["git_commit"] = id.git_commit;
            int cc = post_json(hc, cbody, "firmware confirm");
            net::logf("[mgr] /firmware/confirm -> %d", cc);
            hc.end();
            if (cc >= 200 && cc < 300) s_ota_confirm_pending = false;
        }
    }
    return ok ? ManagerCall::ok(code) : ManagerCall::http_error(code);
}

void worker(void *) {
    // Opt out of the task watchdog: HTTPClient can block for up to the
    // configured timeout (3 s now) and we don't want a single slow
    // POST to brick the device.
    esp_task_wdt_delete(NULL);
    // Slice 4: restore the last applied config from flash before any
    // manager contact, so the device renders its configured layout offline
    // immediately after reboot. Runs here (worker task) because LittleFS +
    // apply_config + ApplyManagedScreens want a real task stack, and the
    // app queue (app::setup) already exists by the time this task runs.
    load_persisted_config();
    uint32_t next_register_attempt_ms = 0;
    uint32_t next_heartbeat_ms = 0;
    uint32_t next_command_poll_ms = 0;
    uint32_t next_manager_http_ms = 0;
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // IP discovery cadence. Independent of the manager: the knob browses
    // _espdisp._tcp every ~15 s so the Select-Display menu fills with
    // mDNS-discovered displays even with no manager provisioned (design §4.1).
    // The browse blocks for the mDNS query window, so it runs only here on the
    // worker task, never on LVGL.
    constexpr uint32_t IP_DISCOVERY_INTERVAL_MS = 15000;
    uint32_t next_ip_discovery_ms = 0;
    // BLE discovery cadence — much slower than IP. BLE is the FALLBACK transport
    // (design §4.3): a peer reachable over IP is controlled over IP, never BLE.
    // The on-demand central scan competes with the radio and holds NimBLE
    // central state, so it runs infrequently (every ~60 s) and only when the
    // last IP browse found nothing — i.e. when there may be a BLE-only peer to
    // reach. ingest_ble_peer is IP-preferred, so even if it runs it never
    // downgrades an IP entry.
    constexpr uint32_t BLE_DISCOVERY_INTERVAL_MS = 60000;
    uint32_t next_ble_discovery_ms = 30000;  // first BLE scan ~30 s after boot
#endif
    for (;;) {
        // Snapshot mutable state under the mutex so CLI updates can't
        // race the HTTP path mid-call.
        lock_state();
        bool have_endpoint = s_endpoint.length() > 0;
        bool prov = is_provisioned();
        bool force = s_force_register;
        unlock_state();

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
        // IP peer discovery runs independently of the manager endpoint so the
        // knob finds + controls displays over mDNS/HTTP with no manager. Gated
        // on WiFi up and skipped during OTA (mDNS query competes for the radio).
        int ip_peers_found = -1;
        if (WiFi.status() == WL_CONNECTED && !net::otaInProgress() &&
            millis() >= next_ip_discovery_ms) {
            ip_peers_found = knob_remote::refresh_ip_peers();  // blocking mDNS browse
            next_ip_discovery_ms = millis() + IP_DISCOVERY_INTERVAL_MS;
        }

        // BLE fallback scan: only when warranted. Run on the slow cadence AND
        // only when the most recent IP browse turned up nothing new (so we go
        // looking for BLE-only peers rather than re-scanning when IP already
        // covers the field). Skipped during OTA — the scan competes for the
        // radio. ip_peers_found == -1 means "no IP browse this tick", which is
        // also fine to fall back from. WiFi does not gate BLE (an off-grid knob
        // with no WiFi is exactly when BLE matters), only OTA does.
        if (!net::otaInProgress() && millis() >= next_ble_discovery_ms && ip_peers_found <= 0) {
            knob_remote::refresh_ble_peers();  // blocking BLE scan, worker task
            next_ble_discovery_ms = millis() + BLE_DISCOVERY_INTERVAL_MS;
        }

        // Drain UI-task-queued remote requests on EVERY tick, before the
        // !have_endpoint early-return below. On a manager-less knob the loop
        // bails out at !have_endpoint, so draining only inside the manager
        // HTTP cycle (or the 15 s discovery gate) stalled a peer-to-peer IP
        // view-switch up to a full discovery interval. These calls are no-ops
        // when nothing is pending and dispatch by the entry's transport (IP or
        // Manager), so a single pair here covers both paths. Gated on WiFi up
        // and not-OTA because the drain does blocking HTTP and the
        // !have_endpoint return we jump in front of was the only WiFi gate.
        if (WiFi.status() == WL_CONNECTED && !net::otaInProgress()) {
            knob_remote::drain_pending_views_fetch();
            knob_remote::drain_pending_switch();
        }
#endif

        if (!have_endpoint) {
            s_health = HealthState::Idle;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        // Skip the entire HTTP cycle while an espota upload is in flight.
        // Heartbeat + register + config-fetch + command-poll all
        // allocate HTTPClient + JSON buffers; doing that while the OTA
        // stream is consuming the WiFi TX queue (and the Update class
        // holds large internal-heap buffers) is the fastest way to
        // disassociate the station mid-upload. Re-check in 1 s.
        if (net::otaInProgress()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t now = millis();
        if (now < next_manager_http_ms) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Translate a ManagerCall status into how long the worker
        // should refuse to make another HTTP call. Bare `int` returns
        // used to mix HTTP codes and HTTPClient error negatives in
        // the same value space - the new switch is exhaustive over
        // ManagerStatus so adding a status forces a deliberate
        // backoff decision here.
        auto apply_backoff = [&](ManagerCall r) -> bool {
            // DEBUG log every non-Ok status so an operator widening
            // the log level can see exactly which preflight or
            // transport leg failed - the new typed status removes
            // the ambiguity the old `int rc` had.
            if (r.status != ManagerStatus::Ok) {
                net::logf_at(net::LOG_DEBUG, "[mgr] status=%s http_code=%d", to_str(r.status),
                             r.http_code);
            }
            // Returns true if the worker should `continue` (skip the
            // rest of this iteration). Sets next_manager_http_ms.
            switch (r.status) {
            case ManagerStatus::Ok:
                return false;
            case ManagerStatus::HttpError:
                // Server replied something non-2xx but didn't refuse
                // transport - don't burn 60s of backoff for a 4xx.
                return false;
            case ManagerStatus::NotProvisioned:
                // Caller already checked this before invoking, so this
                // means state raced. Loop and try again next tick.
                return false;
            case ManagerStatus::WifiDown:
                next_manager_http_ms = millis() + TRANSPORT_FAILURE_BACKOFF_MS;
                return true;
            case ManagerStatus::LowHeap:
                next_manager_http_ms = millis() + LOW_HEAP_BACKOFF_MS;
                return true;
            case ManagerStatus::BeginFailed:
            case ManagerStatus::PayloadError:
            case ManagerStatus::SendFailed:
            case ManagerStatus::BodyTooLarge:
            case ManagerStatus::BodyParseError:
                next_manager_http_ms = millis() + TRANSPORT_FAILURE_BACKOFF_MS;
                return true;
            }
            return false;  // unreachable; -Wswitch-enum should catch missing cases
        };

        if (force || !prov) {
            if (now >= next_register_attempt_ms) {
                ManagerCall r = do_register();
                if (r.is_ok()) {
                    s_force_register = false;
                    next_heartbeat_ms = now + 1000;
                    next_manager_http_ms = 0;
                } else {
                    // Longer delay on real transport errors so we don't
                    // pound an unreachable peer. 401/403 (HttpError) means
                    // the bootstrap provisioning credential is wrong - no
                    // point retrying every 5 s, that just churns the
                    // tiny internal heap. 30 s gives the operator time
                    // to fix the token via `manager-token <tok>`.
                    bool transport = r.burned_transport() || r.status == ManagerStatus::WifiDown;
                    bool auth_problem = r.status == ManagerStatus::HttpError &&
                                        (r.http_code == 401 || r.http_code == 403);
                    uint32_t retry_in = transport ? 10000 : (auth_problem ? 30000 : 5000);
                    next_register_attempt_ms = now + retry_in;
                    apply_backoff(r);
                }
            }
        }
        if (prov && now >= next_heartbeat_ms) {
            ManagerCall r = do_heartbeat();
            next_heartbeat_ms = millis() + s_heartbeat_interval_ms;
            // S5: label this heartbeat's outcome and classify failures so a
            // looping non-2xx heartbeat is diagnosable from /api/diag without
            // guessing what the numeric code meant.
            lock_state();
            s_last_heartbeat_status = to_str(r.status);
            if (r.preflight_refusal())
                s_hb_preflight_refusals++;
            else if (r.burned_transport())
                s_hb_transport_failures++;
            unlock_state();
            if (apply_backoff(r)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        if (prov && s_config_fetch_pending) {
            s_config_fetch_pending = false;
            ManagerCall r = fetch_config();
            if (apply_backoff(r)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        if (prov && now >= next_command_poll_ms) {
            ManagerCall r = poll_commands();
            next_command_poll_ms = millis() + s_command_poll_interval_ms;
            if (apply_backoff(r)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
            // Knob remote enumeration piggybacks the command poll cadence: a
            // cheap GET /devices/summary each poll keeps the Select-Display
            // list fresh without its own task. The pending view-fetch / switch
            // drains run unconditionally near the top of every tick (before the
            // !have_endpoint return), so they are not repeated here.
            knob_remote::refresh_from_manager();
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Waveshare knob remote enumeration + switch. Worker-task only (blocking
// HTTPClient), reusing the same auth/timeout/JSON-into-PSRAM plumbing as the
// register/heartbeat/poll legs above.
constexpr int MAX_REMOTE_LIST_BYTES = 8 * 1024;

int knob_refresh_displays() {
    if (!is_provisioned()) return -1;
    if (WiFi.status() != WL_CONNECTED) return -1;
    if (!manager_heap_ready("knob devices")) return -1;

    HTTPClient http;
    String url = build_url("/devices/summary");
    if (!http.begin(url)) return -1;
    prepare_http(http);
    int code = http.GET();
    if (code != 200) {
        http.end();
        record_error("[mgr] knob devices fetch -> %d", code);
        return -1;
    }
    if (!resp_within_cap(http, MAX_REMOTE_LIST_BYTES, "knob devices")) {
        http.end();
        return -1;
    }
    JsonDocument r(&s_json_allocator);
    bool ok = deserialize_http_json(http, r, "knob devices");
    http.end();
    if (!ok) return -1;

    const char *own = device_identity::get().device_id;
    JsonArray devices = r["devices"].as<JsonArray>();
    knob_remote::begin_ingest();
    int n = 0;
    for (JsonObject d : devices) {
        const char *id = d["id"] | "";
        if (!*id) continue;
        // Skip our own knob entry; entry 0 is the local one already.
        if (own && *own && strcmp(own, id) == 0) continue;
        const char *name = d["name"] | id;
        const char *cur = d["currentScreen"] | "";
        knob_remote::ingest_display(id, name, cur);
        n++;
    }
    knob_remote::commit_ingest();
    return n;
}

bool knob_fetch_views(int dev_idx) {
    if (!is_provisioned()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!manager_heap_ready("knob views")) return false;

    char dev_id[40];
    knob_remote::copy_device_id(dev_idx, dev_id, sizeof(dev_id));
    if (!dev_id[0]) return false;  // local or out-of-range -> nothing to fetch

    HTTPClient http;
    String url = build_url("/devices/") + dev_id + "/views";
    if (!http.begin(url)) return false;
    prepare_http(http);
    int code = http.GET();
    if (code != 200) {
        http.end();
        record_error("[mgr] knob views fetch -> %d", code);
        return false;
    }
    if (!resp_within_cap(http, MAX_REMOTE_LIST_BYTES, "knob views")) {
        http.end();
        return false;
    }
    JsonDocument r(&s_json_allocator);
    bool ok = deserialize_http_json(http, r, "knob views");
    http.end();
    if (!ok) return false;

    JsonArray views = r["views"].as<JsonArray>();
    const char *cur = r["current"] | "";
    // Build bounded id/title arrays for the ingest call. kMaxViews is small;
    // these fixed arrays live on the worker stack (worker has an 8 KB+ stack).
    const char *ids[knob_remote::kMaxViews] = {nullptr};
    const char *titles[knob_remote::kMaxViews] = {nullptr};
    int count = 0;
    for (JsonObject v : views) {
        if (count >= (int)knob_remote::kMaxViews) break;
        const char *vid = v["id"] | "";
        if (!*vid) continue;
        ids[count] = vid;
        titles[count] = v["title"] | vid;
        count++;
    }
    knob_remote::set_views(dev_idx, ids, titles, count, cur);
    return true;
}

bool knob_post_screen_set(const char *device_id, const char *screen_id) {
    if (!device_id || !*device_id || !screen_id || !*screen_id) return false;
    if (!is_provisioned()) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!manager_heap_ready("knob screen.set")) return false;

    HTTPClient http;
    String url = build_url("/devices/") + device_id + "/command";
    if (!http.begin(url)) return false;
    prepare_http(http, /*json=*/true);
    JsonDocument body(&s_json_allocator);
    body["type"] = "screen.set";
    body["payload"]["screen"] = screen_id;
    int code = post_json(http, body, "knob screen.set");
    http.end();
    if (code >= 200 && code < 300) {
        net::logf("[mgr] knob screen.set %s -> %s (code %d)", device_id, screen_id, code);
        return true;
    }
    record_error("[mgr] knob screen.set %s -> code %d", device_id, code);
    return false;
}
#endif  // BOARD_ID_WAVESHARE_KNOB_1_8

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
    net::logf("[mgr] %s endpoint=%s token=%s", s_endpoint.length() ? "configured" : "idle",
              s_endpoint.c_str(), s_token.length() ? "set" : "none");
    if (!s_task) {
        xTaskCreatePinnedToCore(worker, "mgr", 16384, nullptr, 1, &s_task, 0);
    }
}

bool is_provisioned() {
    return s_endpoint.length() > 0 && s_token.length() > 0;
}

void request_config_fetch() {
    // The manager worker drains this on its next loop (~500 ms), giving a
    // near-instant config apply instead of waiting for the command poll.
    s_config_fetch_pending = true;
}

Status status() {
    Status s;
    s.auth = s_auth;
    s.health = s_health;
    s.endpoint = s_endpoint;
    s.endpoint_host = "";
    s.endpoint_base_path = "";
    s.endpoint_port = 0;
    s.endpoint_tls = false;
    s.discovery_method = manager_endpoint::discovery_method_to_string(s_discovery_method);
    manager_endpoint::Endpoint ep;
    if (manager_endpoint::parse_url(std::string(s_endpoint.c_str()), ep)) {
        s.endpoint_host = ep.host.c_str();
        s.endpoint_base_path = ep.base_path.c_str();
        s.endpoint_port = ep.port;
        s.endpoint_tls = ep.tls;
    }
    s.has_token = s_token.length() > 0;
    s.has_sk_token = s_sk_token.length() > 0;
    s.last_register_ms = s_last_register_ms;
    s.last_register_code = s_last_register_code;
    s.last_heartbeat_ms = s_last_heartbeat_ms;
    s.last_heartbeat_code = s_last_heartbeat_code;
    s.last_heartbeat_status = s_last_heartbeat_status;
    s.heartbeat_preflight_refusals = s_hb_preflight_refusals;
    s.heartbeat_transport_failures = s_hb_transport_failures;
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
                  st.health == HealthState::Heartbeating  ? "hb"
                  : st.health == HealthState::Registering ? "reg"
                  : st.health == HealthState::Failed      ? "failed"
                                                          : "idle",
                  st.endpoint.length() ? st.endpoint.c_str() : "(none)",
                  st.has_token ? "set" : "none", st.has_sk_token ? "set" : "none");
        net::logf("[mgr] endpoint host=%s port=%u tls=%d base=%s discovery=%s",
                  st.endpoint_host.length() ? st.endpoint_host.c_str() : "-",
                  (unsigned)st.endpoint_port, (int)st.endpoint_tls,
                  st.endpoint_base_path.length() ? st.endpoint_base_path.c_str() : "-",
                  st.discovery_method.length() ? st.discovery_method.c_str() : "none");
        net::logf("[mgr] last_register=%dms ago code=%d  "
                  "last_hb=%dms ago code=%d",
                  (int)(millis() - st.last_register_ms), st.last_register_code,
                  (int)(millis() - st.last_heartbeat_ms), st.last_heartbeat_code);
        net::logf("[mgr] cfg ver=%s hash=%s  pending_cmds=%u  "
                  "last_cmd=%s/%s -> %s (%lums ago)",
                  st.config_version.length() ? st.config_version.c_str() : "-",
                  st.config_hash.length() ? st.config_hash.c_str() : "-",
                  (unsigned)st.pending_cmd_count,
                  st.last_cmd_id.length() ? st.last_cmd_id.c_str() : "-",
                  st.last_cmd_type.length() ? st.last_cmd_type.c_str() : "-",
                  st.last_cmd_result.length() ? st.last_cmd_result.c_str() : "-",
                  st.last_cmd_ms ? (unsigned long)(millis() - st.last_cmd_ms) : 0UL);
        net::logf("[mgr] ota in_flight=%d job=%s confirm_pending=%d", (int)st.ota_in_flight,
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
        s_discovery_method = manager_endpoint::DiscoveryMethod::Manual;
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
        net::logf("[mgr] token %s", s_token.length() ? "saved" : "cleared");
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
        net::logf("[mgr] sk_token %s (len=%u)", s_sk_token.length() ? "saved" : "cleared",
                  (unsigned)s_sk_token.length());
        s_force_register = true;
        return true;
    }
    if (line == "manager-forget") {
        lock_state();
        s_endpoint = "";
        s_token = "";
        s_sk_token = "";
        s_discovery_method = manager_endpoint::DiscoveryMethod::None;
        s_auth = AuthState::Unprovisioned;
        save_prefs();
        unlock_state();
        net::logf("[mgr] forgotten");
        return true;
    }
    if (line == "manager-discover") {
        // Spec 18 §4: query the LAN for _yeyboats-mgmt._tcp services.
        // The plugin advertises the same service when running on a
        // SignalK host with mDNS enabled. We log every hit; if exactly
        // one is found, auto-set the endpoint and trigger a register.
        if (WiFi.status() != WL_CONNECTED) {
            net::logf("[mgr] discover: wifi not connected");
            return true;
        }
        int n = MDNS.queryService("yeyboats-mgmt", "tcp");
        net::logf("[mgr] discover: %d hit(s) for _yeyboats-mgmt._tcp", n);
        for (int i = 0; i < n; ++i) {
            String host = MDNS.hostname(i);
            uint16_t port = MDNS.port(i);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
            IPAddress ip = MDNS.address(i);  // ESPmDNS renamed IP() -> address() in Arduino 3.x
#else
            IPAddress ip = MDNS.IP(i);
#endif
            net::logf("[mgr]   [%d] %s @ %s:%u", i, host.c_str(), ip.toString().c_str(),
                      (unsigned)port);
        }
        if (n == 1) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
            IPAddress ip = MDNS.address(0);
#else
            IPAddress ip = MDNS.IP(0);
#endif
            uint16_t port = MDNS.port(0);
            // Prefer the resolved IP - .local hostnames re-resolve at
            // every HTTP call which is wasteful and flaky on iOS hot-
            // spots that filter mDNS.
            String url = String("http://") + ip.toString() + ":" + String(port);
            lock_state();
            s_endpoint = url;
            s_discovery_method = manager_endpoint::DiscoveryMethod::Mdns;
            if (!s_token.length()) s_auth = AuthState::Unprovisioned;
            save_prefs();
            s_force_register = true;
            unlock_state();
            net::logf("[mgr] discover: endpoint set to %s; will register", url.c_str());
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
            net::logf("[mgr]   [%u] t=%lums %s", (unsigned)i, (unsigned long)buf[i].timestamp_ms,
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
                  s_render_plan->layout_variant[0] ? s_render_plan->layout_variant : "(none)",
                  (unsigned)s_render_plan->widget_count, (unsigned)s_render_plan->screen_count,
                  (unsigned)s_render_plan->display_width, (unsigned)s_render_plan->display_height,
                  s_applied_config_hash.length() ? s_applied_config_hash.c_str() : "(none)",
                  (unsigned)manager_screens::managed_count());
        for (uint8_t i = 0; i < s_render_plan->screen_count; ++i) {
            const auto &sc = s_render_plan->screens[i];
            net::logf("[mgr]   screen[%u] id=%s tiles=%u", (unsigned)i, sc.id,
                      (unsigned)sc.tile_count);
        }
        return true;
    }
    if (line == "manager-widgets") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        for (uint8_t i = 0; i < s_render_plan->widget_count; ++i) {
            const auto &w = s_render_plan->widgets[i];
            net::logf("[mgr] widget[%u] id=%s type=%s path=%s "
                      "title=%s unit=%s prec=%u fs=%u",
                      (unsigned)i, w.id, manager_config::widget_type_to_string(w.type), w.path,
                      w.title, w.unit, (unsigned)w.precision,
                      (unsigned)(w.style.font_size ? w.style.font_size : w.style.value_font_size));
        }
        return true;
    }
    if (line == "manager-config-dump") {
        if (!s_render_plan_valid) {
            net::logf("[mgr] no render plan applied yet");
            return true;
        }
        JsonDocument out(&s_json_allocator);
        out["layout_variant"] = s_render_plan->layout_variant;
        out["widget_variant"] = s_render_plan->widget_variant;
        out["display"]["width"] = s_render_plan->display_width;
        out["display"]["height"] = s_render_plan->display_height;
        out["widget_count"] = s_render_plan->widget_count;
        out["screen_count"] = s_render_plan->screen_count;
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
            net::logf("[font]   [%u] = %u", (unsigned)i, (unsigned)font_resolver::DEFAULT_SIZES[i]);
        }
        // Demo a few resolve calls.
        uint16_t probes[] = {10, 16, 22, 30, 42, 80};
        for (uint16_t p : probes) {
            net::logf("[font]   resolve(%u) -> %u", (unsigned)p,
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
