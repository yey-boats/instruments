#include "web.h"

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include "storage.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>

#include "net.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ui_screens.h"
#include "board.h"
#include "board_pins.h"
#include "wifi_store.h"
#include "boat_data.h"
#include "source_nmea_wifi.h"
#include "source_nmea2000.h"
#include "input_test.h"
#include "cmd_catalog.h"
#include "error_log.h"
#include "manager.h"
#include "ui_data.h"
#include "screenshot.h"
#include "app_events.h"
#include "config_runtime.h"
#include <esp_heap_caps.h>
#include "build_version.h"
#include "psram_json.h"
#include "proto_target.h"
#include "proto/proto.h"
#include "generated_midl_manifest.h"
#include "midl_render.h"
#include "midl_demo_doc.h"
#include <freertos/semphr.h>

// Gesture diagnostics live in main.cpp (top-level - not in any namespace).
extern "C" {
uint32_t main_gesture_count();
uint32_t main_gesture_suppressed();
const char *main_last_gesture();
void main_touch_state(int *x, int *y, int *pressed, uint32_t *last_ms);
uint32_t main_i2c_err_count();
uint32_t main_i2c_ok_count();
uint32_t main_gt_ready_count();
uint32_t main_gt_points_count();
uint32_t main_touch_irq_count();
const char *main_touch_mode();
}

namespace web {

static WebServer server(80);
static DNSServer dns;
static bool started = false;
static bool captive_active = false;  // only true when in AP mode
static net::WifiState s_bound_state = net::WifiState::Idle;

// Probe URLs the major OSes hit to detect captive portals. The trick: if
// they DON'T get the exact expected response, the OS pops "Sign in to
// network" and opens the captive browser pointing at the URL whose
// response triggered the detection. So we serve the config page inline
// rather than a 302 (some captive browsers refuse to follow redirects).
static bool is_captive_probe_path(const String &p) {
    return p == "/generate_204" || p == "/gen_204" || p == "/hotspot-detect.html" ||
           p == "/library/test/success.html" || p == "/connecttest.txt" || p == "/ncsi.txt" ||
           p == "/success.txt" || p == "/redirect" || p == "/chat" ||
           p == "/check_network_status.txt";
}

// Forward decl: INDEX_HTML[] is defined further down (the big R"HTML(...)"
// block). Both forward decl and definition need consistent linkage.
extern const char INDEX_HTML[];

static void send_captive_page(const char *why) {
    net::logf("[web] captive serve uri=%s host=%s why=%s", server.uri().c_str(),
              server.hostHeader().c_str(), why);
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.send(200, "text/html", FPSTR(INDEX_HTML));
}

// ---- helpers -----------------------------------------------------------

static void send_json(int code, const String &body) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

static void send_json(int code, JsonDocument &doc) {
    String out;
    serializeJson(doc, out);
    send_json(code, out);
}

// Internal-heap watermark below which we refuse expensive endpoints so a
// transient burst doesn't tip lwIP's TX-buffer pool over the cliff and
// drop the AP association. 8 KB largest + 12 KB total is conservative;
// the WiFi driver typically needs ~6 KB contiguous for a TX buffer.
//
// Cheap endpoints (auth check, simple commands) skip this guard - the
// goal is to protect the device from blasting a 460 KB BMP or building
// a 30+ field /api/state doc while the heap is already low. Returns
// true if the request should proceed; returns false (and sends a 503)
// when the breaker trips.
// Tiered heap-low circuit breaker. Heavy endpoints get a higher
// threshold because their internal-heap working set is larger. /api/state
// is the diagnostic surface that has to stay readable even when the
// device is in trouble - it only allocates ~1 KB of String for the
// serialized JSON (the doc itself is in PSRAM via psram_json) so it
// gets the lowest threshold.
enum HeapWeight {
    HEAP_LIGHT = 0,   // /api/state, small JSON responses
    HEAP_MEDIUM = 1,  // /api/diag, /api/logs
    HEAP_HEAVY = 2,   // /api/screenshot.bmp (460 kB BMP)
};

static bool heap_ok(HeapWeight weight) {
    // During an inbound OTA upload, all but the lightest endpoints back
    // off. The Update class holds 4-8 kB of internal-heap working
    // buffers and the WiFi TX queue is saturated.
    if (net::otaInProgress() && weight != HEAP_LIGHT) {
        server.sendHeader("Retry-After", "10");
        server.send(503, "text/plain", "ota upload in progress; try again shortly");
        return false;
    }
    size_t min_free, min_block;
    switch (weight) {
    case HEAP_LIGHT:
        min_free = 4 * 1024;
        min_block = 3 * 1024;
        break;
    case HEAP_MEDIUM:
        min_free = 8 * 1024;
        min_block = 6 * 1024;
        break;
    case HEAP_HEAVY:
    default:
        // The screenshot endpoint allocates its 460 kB BMP in PSRAM, so
        // the internal-heap need is just lwIP's TX buffer pool while
        // streaming (~6 kB contiguous per packet). The device's
        // steady-state largest internal block sits around 7.6 kB after
        // LVGL + NimBLE + WiFi finish initializing - 8 kB would 503
        // every heavy request, so use 6 kB as the floor.
        min_free = 10 * 1024;
        min_block = 6 * 1024;
        break;
    }
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_int >= min_free && largest >= min_block) return true;
    net::logf_at(net::LOG_WARN,
                 "[web] 503 heap-breaker uri=%s weight=%d free=%u largest=%u "
                 "min_free=%u min_block=%u",
                 server.uri().c_str(), (int)weight, (unsigned)free_int, (unsigned)largest,
                 (unsigned)min_free, (unsigned)min_block);
    server.sendHeader("Retry-After", "5");
    server.send(503, "text/plain", "low internal heap; try again shortly");
    return false;
}

static bool api_auth_required() {
    storage::Namespace p("web", true);
    return p.get_u8("auth", 0) != 0;
}

static bool require_api_auth() {
#ifdef YEYBOATS_LAB_OPEN_WEB
    // ---- LAB-ONLY, TEMPORARY: web API auth bypass ------------------------
    // Compiled in ONLY when the build explicitly defines YEYBOATS_LAB_OPEN_WEB
    // (e.g. PLATFORMIO_BUILD_FLAGS="-D YEYBOATS_LAB_OPEN_WEB=1"). Production
    // builds never define it, so this branch compiles out and normal Basic
    // Auth applies. Used to capture headless /api/screenshot.png on a bench
    // device whose web password is unknown. Re-secure by reflashing a normal
    // build (the NVS web/{auth,user,pass} are left untouched).
    // See docs/lab/temporary-web-auth-bypass.md. DO NOT SHIP.
    return true;
#else
    if (!api_auth_required()) return true;
    storage::Namespace p("web", true);
    String user = String(p.get_string("user", "espdisp").c_str());
    String pass = String(p.get_string("pass", "").c_str());
    if (user.length() == 0 || pass.length() == 0) return true;
    if (server.authenticate(user.c_str(), pass.c_str())) return true;
    server.requestAuthentication(BASIC_AUTH, "espdisp", "auth required");
    return false;
#endif
}

// ---- /api/state --------------------------------------------------------

// Build the core state document - what UI polls every few seconds. Cheap
// to serialize, no LVGL state reads, no log-ring iteration. Heavy
// forensic fields (touch counters, queue depths, gestures, recentErrors,
// manager.lastCmd*) moved to /api/diag below so they don't run on
// every poll.
static void build_state_doc(JsonDocument &doc) {
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["id"] = net::deviceId();
    dev["uptime_ms"] = (uint32_t)millis();
    dev["heap_free"] = (uint32_t)ESP.getFreeHeap();
    dev["psram_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // Fragmentation + watermark visibility for diagnosing slow leaks /
    // big-alloc failures (the symptom is "WiFi quietly disassociates"
    // because lwIP can't get a TX buffer). Cheap to read.
    dev["heap_internal_free"] =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    dev["heap_internal_largest"] =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Internal-heap watermark specifically: esp_get_minimum_free_heap_size()
    // spans all heaps (PSRAM-inclusive) and reports ~7.8 M here, masking the
    // real internal pressure. The internal-only minimum is the number that
    // predicts WiFi/lwIP allocation failures.
    dev["heap_min_ever"] =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    {
        float tC = board::chipTempC();
        if (!isnan(tC)) dev["chip_temp_c"] = tC;
    }
    // __DATE__ __TIME__ is the per-TU compile timestamp; kept for back
    // compat with tools/ota_flash.sh which greps it out of the binary
    // to verify a flash landed. The richer fields below come from
    // include/build_version.h (regenerated by tools/version.py each
    // build): `firmware_version` is the full release/dev string,
    // `firmware_base_version` is the bare semver, and `build_iso` is
    // the wall-clock build timestamp.
    dev["build"] = __DATE__ " " __TIME__;
    dev["firmware_version"] = FW_VERSION;
    dev["firmware_base_version"] = FW_BASE_VERSION;
    dev["firmware_version_source"] = FW_VERSION_SOURCE;
    dev["git_commit"] = FW_GIT_COMMIT;
    dev["build_iso"] = FW_BUILD_ISO;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["up"] = net::wifiUp();
    wifi["mode"] = net::wifiUp() ? "STA" : "AP";
    wifi["state"] = net::wifiStateName();
    wifi["ssid"] = WiFi.SSID();
    wifi["ip"] = net::ipString();
    wifi["rssi"] = net::rssi();

    JsonObject sk = doc["sk"].to<JsonObject>();
    sk["state"] = sk::connectionStatus();
    {
        storage::Namespace p("sk", true);
        sk["host"] = p.get_string("host", "");
        sk["port"] = p.get_u32("port", 3000);
        // Never echo the token itself - only whether one is configured.
        // CLAUDE.md: "the SignalK token field is sensitive."
        sk["has_token"] = !p.get_string("token", "").empty();
    }
    // Task diagnostics: confirms sk_task is alive (iters keeps growing)
    // and exposes the peak ws.loop() duration since last sample. A high
    // peak here is fine - it's on core 0, not on the LVGL task.
    sk["task_iters"] = sk::loopIters();
    sk["task_peak_us"] = sk::loopMaxUs();

    {
        JsonObject web_auth = doc["webAuth"].to<JsonObject>();
        web_auth["enabled"] = api_auth_required();
    }

    {
        manager::Status st = manager::status();
        JsonObject mgr = doc["manager"].to<JsonObject>();
        mgr["deviceId"] = st.device_id;
        mgr["registered"] = st.has_token && st.endpoint.length() > 0;
        mgr["lastHeartbeatOk"] = st.last_heartbeat_code >= 200 && st.last_heartbeat_code < 300;
        mgr["endpoint"] = st.endpoint;
        mgr["endpointHost"] = st.endpoint_host;
        mgr["endpointPort"] = st.endpoint_port;
        mgr["endpointTls"] = st.endpoint_tls;
        mgr["endpointBasePath"] = st.endpoint_base_path;
        mgr["discoveryMethod"] = st.discovery_method;
        mgr["hasToken"] = st.has_token;
        mgr["hasSkToken"] = st.has_sk_token;
        mgr["lastRegisterCode"] = st.last_register_code;
        mgr["lastHeartbeatCode"] = st.last_heartbeat_code;
        // S5: labelled outcome + classified failure counters so a looping
        // non-2xx heartbeat is diagnosable (pre-flight refusal vs transport).
        mgr["lastHeartbeatStatus"] = st.last_heartbeat_status;
        mgr["heartbeatPreflightRefusals"] = st.heartbeat_preflight_refusals;
        mgr["heartbeatTransportFailures"] = st.heartbeat_transport_failures;
        mgr["heartbeatIntervalMs"] = st.heartbeat_interval_ms;
        mgr["commandPollIntervalMs"] = st.command_poll_interval_ms;
        mgr["configVersion"] = st.config_version;
        mgr["configHash"] = st.config_hash;
        mgr["health"] = st.health == manager::HealthState::Heartbeating  ? "heartbeating"
                        : st.health == manager::HealthState::Registering ? "registering"
                        : st.health == manager::HealthState::Failed      ? "failed"
                                                                         : "idle";
        // Manager keeps a couple of fields on the hot path because the
        // web UI shows them prominently. The verbose command/firmware
        // diagnostics + recent-errors list are in /api/diag.
    }

    JsonObject screen = doc["screen"].to<JsonObject>();
    screen["index"] = ui::current_index();
    screen["id"] = ui::current_id();
    screen["title"] = ui::current_title();
    screen["count"] = (uint32_t)ui::screen_count();

    // Display state: brightness + theme. Consumed by spec 17 F4 tests
    // (manager.brightness.set/theme.set commands assert /api/state.display
    // reflects the new value).
    JsonObject display = doc["display"].to<JsonObject>();
    display["brightness"] = (uint32_t)ui::brightness();
    JsonObject ui_o = doc["ui"].to<JsonObject>();
    {
        storage::Namespace pu("ui", true);
        ui_o["theme"] = pu.get_string("theme", "night");
    }
}

static void handle_state() {
    if (!require_api_auth()) return;
    if (!heap_ok(HEAP_LIGHT)) return;
    JsonDocument doc(&yeyboats::psram_json);
    build_state_doc(doc);
    send_json(200, doc);
}

// /api/diag - debug/forensic fields that aren't worth polling at the
// /api/state cadence. Touch counters, queue depths, gesture stats,
// manager command + OTA detail, recent error ring. The web UI only
// hits this when a diagnostics view is open; the lab logger fetches
// it on demand.
static void handle_diag() {
    if (!require_api_auth()) return;
    if (!heap_ok(HEAP_MEDIUM)) return;
    JsonDocument doc(&yeyboats::psram_json);

    {
        manager::Status st = manager::status();
        JsonObject mgr = doc["manager"].to<JsonObject>();
        // Spec 17 §11 command diagnostics
        mgr["pendingCmdCount"] = (uint32_t)st.pending_cmd_count;
        if (st.last_cmd_id.length()) mgr["lastCmdId"] = st.last_cmd_id;
        if (st.last_cmd_type.length()) mgr["lastCmdType"] = st.last_cmd_type;
        if (st.last_cmd_result.length()) mgr["lastCmdResult"] = st.last_cmd_result;
        if (st.last_cmd_ms) {
            mgr["lastCmdAgeMs"] = (uint32_t)(millis() - st.last_cmd_ms);
        }
        // Spec 17 §11 firmware update state
        mgr["otaInFlight"] = st.ota_in_flight;
        mgr["otaConfirmPending"] = st.ota_confirm_pending;
        if (st.ota_job_id.length()) mgr["otaJobId"] = st.ota_job_id;
        // Spec 17 §5 recent errors
        JsonArray errs = mgr["recentErrors"].to<JsonArray>();
        error_log::Entry buf[error_log::MAX_ENTRIES];
        size_t n = error_log::copy(buf, error_log::MAX_ENTRIES);
        for (size_t i = 0; i < n; ++i) {
            JsonObject e = errs.add<JsonObject>();
            e["t_ms"] = buf[i].timestamp_ms;
            e["msg"] = buf[i].message;
        }
    }

    JsonObject queues = doc["queues"].to<JsonObject>();
    queues["ui_depth"] = (uint32_t)app::ui_queue_depth();
    queues["ui_hi"] = app::ui_high_water();
    queues["net_depth"] = (uint32_t)app::net_queue_depth();
    queues["net_hi"] = app::net_high_water();

    JsonObject gestures = doc["gestures"].to<JsonObject>();
    gestures["count"] = ::main_gesture_count();
    gestures["suppressed"] = ::main_gesture_suppressed();
    gestures["last"] = ::main_last_gesture();

    int tx = -1, ty = -1, tp = 0;
    uint32_t tlast = 0;
    ::main_touch_state(&tx, &ty, &tp, &tlast);
    JsonObject touch = doc["touch"].to<JsonObject>();
    touch["x"] = tx;
    touch["y"] = ty;
    touch["pressed"] = tp;
    touch["last_ms"] = tlast;
    touch["mode"] = ::main_touch_mode();
    touch["irq"] = ::main_touch_irq_count();
    touch["i2c_ok"] = ::main_i2c_ok_count();
    touch["i2c_err"] = ::main_i2c_err_count();
    touch["gt_ready"] = ::main_gt_ready_count();
    touch["gt_points"] = ::main_gt_points_count();

    send_json(200, doc);
}

// ---- /api/screens, /api/screen/<id> ------------------------------------

static void handle_screens() {
    if (!require_api_auth()) return;
    JsonDocument doc(&yeyboats::psram_json);
    JsonArray arr = doc.to<JsonArray>();
    int active = ui::current_index();
    for (size_t i = 0; i < ui::screen_count(); ++i) {
        const char *id = "?";
        const char *title = "?";
        bool hidden = false;
        ui::screen_info((int)i, &id, &title, &hidden);
        JsonObject s = arr.add<JsonObject>();
        s["index"] = (uint32_t)i;
        s["id"] = id;
        s["title"] = title;
        s["hidden"] = hidden;
        s["active"] = (active == (int)i);
    }
    send_json(200, doc);
}

static void handle_screen_set() {
    if (!require_api_auth()) return;
    String id = server.uri();
    int slash = id.lastIndexOf('/');
    if (slash < 0) {
        server.send(400, "text/plain", "missing id");
        return;
    }
    id = id.substring(slash + 1);
    // Queue the screen change for the UI task. ui::current_index/id read
    // here is "current at request time" - response is best-effort and
    // not synchronised with the change applying.
    app::Command cmd;
    cmd.type = app::CommandType::ShowScreen;
    strncpy(cmd.a, id.c_str(), sizeof(cmd.a) - 1);
    if (!app::post(cmd, 50)) {
        server.send(503, "text/plain", "ui queue full");
        return;
    }
    JsonDocument doc(&yeyboats::psram_json);
    doc["queued"] = true;
    doc["target"] = id;
    send_json(202, doc);
}

// ---- /api/sk -----------------------------------------------------------

static void put_double(JsonObject o, const char *k, double v) {
    if (!isnan(v)) o[k] = v;
}

static void handle_sk_data() {
    if (!require_api_auth()) return;
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    JsonDocument doc(&yeyboats::psram_json);
    JsonObject nav = doc["nav"].to<JsonObject>();
    put_double(nav, "lat", d.lat);
    put_double(nav, "lon", d.lon);
    put_double(nav, "sog", d.sog);
    put_double(nav, "stw", d.stw);
    put_double(nav, "cog", d.cogTrue);
    put_double(nav, "hdg", d.headingTrue);

    JsonObject wind = doc["wind"].to<JsonObject>();
    put_double(wind, "awa", d.awa);
    put_double(wind, "aws", d.aws);
    put_double(wind, "twa", d.twa);
    put_double(wind, "tws", d.tws);

    JsonObject env = doc["env"].to<JsonObject>();
    put_double(env, "depth", d.depth);
    put_double(env, "waterTemp", d.waterTemp);

    JsonObject elec = doc["electrical"].to<JsonObject>();
    put_double(elec, "battVoltage", d.battVoltage);
    put_double(elec, "battSoc", d.battSoc);
    put_double(elec, "tankFuel", d.tankFuel);
    put_double(elec, "tankWater", d.tankWater);

    JsonObject route = doc["route"].to<JsonObject>();
    put_double(route, "xte", d.xte);
    put_double(route, "cts", d.cts);
    put_double(route, "btw", d.btw);
    put_double(route, "dtw", d.dtw);
    put_double(route, "vmg", d.vmg);

    JsonObject ap = doc["autopilot"].to<JsonObject>();
    put_double(ap, "target", d.apTargetHdg);
    if (d.apState[0]) ap["state"] = d.apState;

    doc["connected"] = d.connected;
    doc["lastUpdateAgeMs"] = d.lastUpdateMs ? (uint32_t)(millis() - d.lastUpdateMs) : (uint32_t)0;

    send_json(200, doc);
}

// ---- /api/boat ---------------------------------------------------------
// Per-field source / age / freshness view onto boat::Snapshot. Tests
// use this to verify that priority + freshness routes the right
// source-of-truth to renderers.

static void handle_boat() {
    if (!require_api_auth()) return;
    boat::Snapshot s;
    boat::copy_snapshot(s);
    boat::Timeouts t = boat::get_timeouts();
    boat::Priority p = boat::get_priority();
    uint32_t now = millis();

    JsonDocument doc(&yeyboats::psram_json);
    JsonObject prio = doc["priority"].to<JsonObject>();
    JsonArray order = prio["order"].to<JsonArray>();
    for (uint8_t i = 0; i < 5; ++i) {
        if (p.order[i] == boat::SourceKind::None) break;
        order.add(boat::source_name(p.order[i]));
    }
    JsonObject to = prio["timeouts_ms"].to<JsonObject>();
    to["nmea2000"] = t.nmea2000_ms;
    to["nmea_wifi"] = t.nmea_wifi_ms;
    to["signalk"] = t.signalk_ms;
    to["demo"] = t.demo_ms;

    JsonObject fields = doc["fields"].to<JsonObject>();
    auto emit = [&](const char *name, const boat::Field &f) {
        JsonObject o = fields[name].to<JsonObject>();
        if (!isnan(f.value)) o["value"] = f.value;
        o["source"] = boat::source_name(f.source);
        if (f.updated_ms) o["age_ms"] = (uint32_t)(now - f.updated_ms);
        o["fresh"] = boat::fresh(f, now, boat::timeout_for(t, f.source));
    };
    emit("lat_deg", s.lat_deg);
    emit("lon_deg", s.lon_deg);
    emit("sog_mps", s.sog_mps);
    emit("stw_mps", s.stw_mps);
    emit("cog_true_rad", s.cog_true_rad);
    emit("heading_true_rad", s.heading_true_rad);
    emit("awa_rad", s.awa_rad);
    emit("aws_mps", s.aws_mps);
    emit("twa_rad", s.twa_rad);
    emit("tws_mps", s.tws_mps);
    emit("depth_m", s.depth_m);
    emit("water_temp_k", s.water_temp_k);
    emit("battery_v", s.battery_v);
    emit("battery_soc", s.battery_soc);
    emit("xte_m", s.xte_m);
    emit("btw_rad", s.btw_rad);
    emit("dtw_m", s.dtw_m);

    JsonObject ap = doc["autopilot_state"].to<JsonObject>();
    if (s.autopilot_state[0]) ap["value"] = s.autopilot_state;
    ap["source"] = boat::source_name(s.autopilot_state_source);
    if (s.autopilot_state_updated_ms) {
        ap["age_ms"] = (uint32_t)(now - s.autopilot_state_updated_ms);
    }

    JsonObject sources = doc["sources"].to<JsonObject>();
    {
        auto st = nmea_wifi::status();
        JsonObject nw = sources["nmea_wifi"].to<JsonObject>();
        nw["enabled"] = st.enabled;
        nw["proto"] = st.proto == nmea_wifi::Protocol::Tcp ? "tcp" : "udp";
        nw["host"] = st.host;
        nw["port"] = st.port;
        nw["connected"] = st.connected;
        nw["bytes_in"] = st.bytes_in;
        nw["sentences_ok"] = st.sentences_ok;
        nw["sentences_bad"] = st.sentences_bad;
        nw["last_rx_age_ms"] = st.last_rx_ms ? (uint32_t)(now - st.last_rx_ms) : 0;
    }
    {
        auto st = nmea2000::status();
        JsonObject n2 = sources["nmea2000"].to<JsonObject>();
        n2["compiled_in"] = st.compiled_in;
        n2["enabled"] = st.enabled;
        n2["sniff"] = st.sniff;
        n2["tx_enabled"] = st.tx_enabled;
        n2["rx_pin"] = st.rx_pin;
        n2["tx_pin"] = st.tx_pin;
        n2["frames_rx"] = st.frames_rx;
        n2["pgns_decoded"] = st.pgns_decoded;
        n2["pgns_unknown"] = st.pgns_unknown;
        n2["last_rx_age_ms"] = st.last_rx_ms ? (uint32_t)(now - st.last_rx_ms) : 0;
    }

    send_json(200, doc);
}

// ---- /api/commands -----------------------------------------------------
// Machine-readable catalog of every console command the firmware
// accepts. Useful for tooling, plugin auto-discovery, and the
// /help/commands HTML page rendered below.

static void handle_commands_json() {
    if (!require_api_auth()) return;
    JsonDocument doc(&yeyboats::psram_json);
    JsonArray arr = doc["commands"].to<JsonArray>();
    const auto *list = cmd_catalog::entries();
    size_t n = cmd_catalog::entry_count();
    for (size_t i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["category"] = list[i].category;
        o["syntax"] = list[i].syntax;
        o["summary"] = list[i].summary;
        o["http"] = list[i].http;
        o["ble_serial"] = list[i].ble_serial;
    }
    doc["count"] = n;
    doc["note"] = "Commands with http=false are reachable only over BLE "
                  "NUS / USB serial. /api/cmd returns 403 for those.";
    send_json(200, doc);
}

// HTML help page - groups by category, marks BLE/serial-only commands.
static void handle_commands_html() {
    String html;
    html.reserve(8 * 1024);
    html += F("<!doctype html>\n<meta charset=\"utf-8\">\n"
              "<title>Yey Boats Instruments - commands</title>\n"
              "<style>"
              "body{font:14px/1.5 system-ui,sans-serif;background:#05101c;"
              "color:#eaf2ff;margin:0;padding:16px;max-width:1080px}"
              "h1{margin:0 0 4px 0;font-size:20px;color:#9ec5fe}"
              "h2{margin:18px 0 6px 0;font-size:14px;color:#7aa9d8;"
              "text-transform:uppercase;letter-spacing:.08em}"
              "p.note{color:#6c8bb1;margin:6px 0}"
              "table{width:100%;border-collapse:collapse;background:#0a2540;"
              "border:1px solid #223a55;border-radius:6px;overflow:hidden}"
              "th,td{padding:6px 10px;border-bottom:1px solid #14304c;"
              "text-align:left;vertical-align:top}"
              "th{background:#11355a;font-weight:600;font-size:12px;"
              "color:#9ec5fe;text-transform:uppercase;letter-spacing:.05em}"
              "code{font-family:ui-monospace,monospace;color:#cae6ff;"
              "background:#05101c;padding:1px 6px;border-radius:3px;"
              "white-space:nowrap}"
              "tr:last-child td{border-bottom:0}"
              ".no-http{color:#ff9b8a}"
              "a{color:#9ec5fe}"
              "</style>\n"
              "<h1>Yey Boats Instruments commands</h1>\n"
              "<p class=\"note\">Reachable from BLE Nordic UART, USB serial, "
              "and (where marked) HTTP <code>POST /api/cmd</code>. "
              "Machine-readable: <a href=\"/api/commands\">/api/commands</a></p>\n");

    const auto *list = cmd_catalog::entries();
    size_t n = cmd_catalog::entry_count();
    const char *cur_cat = nullptr;
    for (size_t i = 0; i < n; ++i) {
        const auto &e = list[i];
        if (cur_cat == nullptr || strcmp(cur_cat, e.category) != 0) {
            if (cur_cat) html += F("</table>\n");
            cur_cat = e.category;
            html += F("<h2>");
            html += e.category;
            html += F("</h2>\n<table><tr><th>Command</th><th>Description</th>"
                      "<th>Transports</th></tr>\n");
        }
        html += F("<tr><td><code>");
        html += e.syntax;
        html += F("</code></td><td>");
        html += e.summary;
        html += F("</td><td>");
        if (e.http) {
            html += F("BLE / serial / HTTP");
        } else {
            html += F("<span class=\"no-http\">BLE / serial only</span>");
        }
        html += F("</td></tr>\n");
    }
    if (cur_cat) html += F("</table>\n");
    html += F("<p class=\"note\">See <a href=\"/\">/</a> for the live state "
              "view and <code>POST /api/cmd</code> for HTTP-allowed commands."
              "</p>\n");

    server.send(200, "text/html", html);
}

// ---- /api/midl/manifest (GET) ------------------------------------------
// Serves the embedded MIDL capabilities manifest verbatim from flash/rodata.
// No heap allocation: the const char* pointer is passed directly to send().

static void handle_midl_manifest() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", midl_manifest::JSON);
}

// ---- MIDL current-doc store --------------------------------------------
// Holds the most-recently-applied MIDL document verbatim (and the one
// before it) as PSRAM char* buffers, mirroring layout_loader.cpp's
// s_last_json pattern. set_current rotates current -> previous so
// /api/midl/reset?to=previous can roll back one step.
//
// IN-RAM ONLY for now: these buffers do NOT survive a reboot. Flash
// persistence of the current MIDL doc is a later phase; on boot the
// device renders the baked factory doc (midl::demo::SQUARE_480_JSON) until
// a new doc is delivered. The mutex guards both buffers + lengths so the
// web task (set/copy) and any future reader don't race.
namespace midl_doc {

static char *s_current = nullptr;
static size_t s_current_len = 0;
static char *s_previous = nullptr;
static size_t s_previous_len = 0;
static SemaphoreHandle_t s_mtx = nullptr;

static inline SemaphoreHandle_t mtx() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    return s_mtx;
}

// Rotate current -> previous, then store a fresh PSRAM copy of [bytes,len)
// as the new current. Returns false (and leaves the store unchanged
// except for the rotation already performed) if the PSRAM alloc fails.
static bool set_current(const char *bytes, size_t len) {
    xSemaphoreTake(mtx(), portMAX_DELAY);
    // Rotate: free the doc two-back, current becomes previous.
    if (s_previous) {
        heap_caps_free(s_previous);
        s_previous = nullptr;
        s_previous_len = 0;
    }
    s_previous = s_current;
    s_previous_len = s_current_len;
    s_current = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!s_current) {
        s_current_len = 0;
        xSemaphoreGive(s_mtx);
        net::logf("[midl] doc store: PSRAM alloc failed (%u bytes)", (unsigned)len);
        return false;
    }
    memcpy(s_current, bytes, len);
    s_current[len] = 0;
    s_current_len = len;
    xSemaphoreGive(s_mtx);
    return true;
}

// Copy the current doc into `out`. Returns false if no current doc.
static bool copy_current(String &out) {
    xSemaphoreTake(mtx(), portMAX_DELAY);
    bool ok = s_current && s_current_len;
    if (ok) out = String(s_current);
    xSemaphoreGive(s_mtx);
    return ok;
}

// Copy the previous doc into `out`. Returns false if no previous doc.
static bool copy_previous(String &out) {
    xSemaphoreTake(mtx(), portMAX_DELAY);
    bool ok = s_previous && s_previous_len;
    if (ok) out = String(s_previous);
    xSemaphoreGive(s_mtx);
    return ok;
}

}  // namespace midl_doc

// ---- /api/midl/config (POST / GET) + /api/midl/reset (POST) ------------
// Device-hosted MIDL delivery. The web handler runs on the WebServer task,
// so it MUST NOT touch LVGL or call midl::render::apply_all directly. It
// only parses + validates the doc (in PSRAM) and queues a ConfigApplyMidl
// app::Command whose blob it hands to the UI task; the pump (app_events.cpp)
// frees the blob after apply. Returning 200 means "accepted + queued +
// stored", not "rendered" — apply is async on the UI task.

// Queue a ConfigApplyMidl command carrying a PSRAM copy of [body,len).
// On success ownership of the blob transfers to the queue (pump frees it).
// Returns 0 on success, or an HTTP status to send on failure.
static int queue_midl_apply(const char *body, size_t len) {
    void *blob = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) return 503;
    memcpy(blob, body, len);
    app::Command cmd;
    cmd.type = app::CommandType::ConfigApplyMidl;
    cmd.blob = blob;
    cmd.blob_len = len;
    if (!app::post(cmd, 50)) {
        heap_caps_free(blob);
        return 503;
    }
    return 0;
}

static void send_err(int code, const char *msg) {
    JsonDocument doc(&yeyboats::psram_json);
    doc["ok"] = false;
    doc["error"] = msg;
    send_json(code, doc);
}

static void handle_midl_config_post() {
    if (!require_api_auth()) return;
    if (!server.hasArg("plain")) {
        send_err(400, "empty body");
        return;
    }
    const String &body = server.arg("plain");
    size_t len = body.length();
    if (len == 0) {
        send_err(400, "empty body");
        return;
    }
    if (len > 48 * 1024) {
        send_err(413, "midl doc too large (48 KB max)");
        return;
    }
    // Parse into a PSRAM-backed document — never on the web-task stack.
    JsonDocument doc(&yeyboats::psram_json);
    DeserializationError perr = deserializeJson(doc, body.c_str(), len);
    if (perr) {
        String e = String("parse: ") + perr.c_str();
        send_err(400, e.c_str());
        return;
    }
    // Lightweight render-safety check (NOT a full schema gate): require a
    // top-level "midl" version marker and at least one usable screen.
    if (doc["midl"].isNull()) {
        send_err(400, "no usable screen / missing 'screens' array");
        return;
    }
    JsonVariantConst screen =
        midl::render::select_screen(doc.as<JsonVariantConst>(), nullptr, nullptr);
    if (screen.isNull()) {
        send_err(400, "no usable screen / missing 'screens' array");
        return;
    }
    // Queue the apply on the UI task (carries a PSRAM blob; pump frees it).
    int qrc = queue_midl_apply(body.c_str(), len);
    if (qrc != 0) {
        send_err(qrc, qrc == 503 ? "ui queue full / blob alloc failed" : "queue failed");
        return;
    }
    // Store the verbatim body as the new current doc (rotates previous).
    midl_doc::set_current(body.c_str(), len);
    JsonDocument out(&yeyboats::psram_json);
    out["ok"] = true;
    out["screens"] = (uint32_t)(doc["screens"].is<JsonArrayConst>() ? doc["screens"].size() : 0);
    send_json(200, out);
}

static void handle_midl_config_get() {
    if (!require_api_auth()) return;
    String body;
    if (!midl_doc::copy_current(body)) {
        send_err(404, "no current doc");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", body);
}

static void handle_midl_reset() {
    if (!require_api_auth()) return;
    String to = server.hasArg("to") ? server.arg("to") : String("default");
    if (to == "previous") {
        String prev;
        if (!midl_doc::copy_previous(prev)) {
            send_err(409, "no previous doc");
            return;
        }
        int qrc = queue_midl_apply(prev.c_str(), prev.length());
        if (qrc != 0) {
            send_err(qrc, "ui queue full / blob alloc failed");
            return;
        }
        // Re-applying previous makes it current again (swap current/previous).
        midl_doc::set_current(prev.c_str(), prev.length());
        JsonDocument out(&yeyboats::psram_json);
        out["ok"] = true;
        out["reset"] = "previous";
        send_json(200, out);
        return;
    }
    // Default (or any unrecognized "to"): the baked factory doc.
    const char *def = midl::demo::SQUARE_480_JSON;
    size_t len = strlen(def);
    int qrc = queue_midl_apply(def, len);
    if (qrc != 0) {
        send_err(qrc, "ui queue full / blob alloc failed");
        return;
    }
    midl_doc::set_current(def, len);
    JsonDocument out(&yeyboats::psram_json);
    out["ok"] = true;
    out["reset"] = "default";
    send_json(200, out);
}

// ---- /api/layout (GET / PUT) -------------------------------------------

static void handle_layout_get() {
    if (!require_api_auth()) return;
    String body;
    if (!layout::copy_last_json(body)) {
        server.send(404, "text/plain", "no layout loaded");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", body);
}

static void handle_layout_put() {
    if (!require_api_auth()) return;
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    const String &body = server.arg("plain");
    size_t len = body.length();
    if (len == 0) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    if (len > 32 * 1024) {
        server.send(413, "text/plain", "layout too large (32 KB max)");
        return;
    }
    // Copy into a PSRAM-backed buffer; ownership transfers to the queue.
    // Pump frees on completion.
    void *blob = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        server.send(503, "text/plain", "blob alloc failed");
        return;
    }
    memcpy(blob, body.c_str(), len);
    app::Command cmd;
    cmd.type = app::CommandType::ApplyLayout;
    cmd.blob = blob;
    cmd.blob_len = len;
    if (!app::post(cmd, 50)) {
        heap_caps_free(blob);
        server.send(503, "text/plain", "ui queue full");
        return;
    }
    JsonDocument doc(&yeyboats::psram_json);
    doc["queued"] = true;
    doc["size"] = (uint32_t)len;
    send_json(202, doc);
}

// ---- /api/dashboard/config.{json,yaml} --------------------------------
// Dashboard config is the operator-facing name for the same layout document
// consumed by /api/layout. JSON is canonical on-device. The .yaml endpoint
// serves and accepts JSON-compatible YAML (JSON syntax is valid YAML 1.2),
// keeping the ESP32 parser small and deterministic.

static void handle_dashboard_config_get_json() {
    handle_layout_get();
}

static void handle_dashboard_config_get_yaml() {
    if (!require_api_auth()) return;
    String body;
    if (!layout::copy_last_json(body)) {
        server.send(404, "text/plain", "no dashboard config loaded");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/yaml", body);
}

static void handle_dashboard_config_put() {
    handle_layout_put();
}

static void handle_security() {
    if (!require_api_auth()) return;
    JsonDocument doc(&yeyboats::psram_json);
    JsonObject web = doc["web"].to<JsonObject>();
    web["bind"] = net::wifiUp() ? "station-ip" : "setup-ap";
    web["auth"] = api_auth_required() ? "basic" : "none-on-device";
    web["basic_auth_enabled"] = api_auth_required();
    web["intended_network"] = "trusted LAN or temporary setup AP";
    web["secrets_echoed"] = false;
    JsonArray webWrite = web["write_endpoints"].to<JsonArray>();
    webWrite.add("/api/dashboard/config.json");
    webWrite.add("/api/dashboard/config.yaml");
    webWrite.add("/api/layout");
    webWrite.add("/api/wifi/connect");
    webWrite.add("/api/cmd");
    webWrite.add("/api/midl/config");
    webWrite.add("/api/midl/reset");
    web["touch_injection_over_http"] = false;

    JsonObject ble = doc["ble"].to<JsonObject>();
    ble["pairing_required"] = false;
    ble["auth"] = "none-in-current-firmware";
    ble["intended_range"] = "local physical proximity";
    ble["configuration_characteristic"] = "boat-mfd CONFIGURATION";
    ble["max_single_write_bytes"] = 512;
    ble["large_dashboard_import"] = "use web or SignalK manager";

    JsonObject signalk = doc["signalk"].to<JsonObject>();
    signalk["manager_auth"] = "SignalK bearer token plus device token";
    signalk["device_pull_header"] = "X-EspDisp-Authorization";
    signalk["dashboard_config_command"] = "config.reload";
    send_json(200, doc);
}

// ---- /api/wifi/scan, /networks, /connect -------------------------------

static bool s_scan_started = false;

static void handle_wifi_scan() {
    if (!require_api_auth()) return;
    // Async kick. Returns immediately; result is fetched via /api/wifi/networks.
    int r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING) {
        send_json(202, "{\"running\":true}");
        return;
    }
    WiFi.scanNetworks(true /* async */, true /* show hidden */);
    s_scan_started = true;
    send_json(202, "{\"running\":true,\"started\":true}");
}

static void handle_wifi_networks() {
    if (!require_api_auth()) return;
    int n = WiFi.scanComplete();
    JsonDocument doc(&yeyboats::psram_json);
    if (n == WIFI_SCAN_RUNNING) {
        doc["running"] = true;
        send_json(200, doc);
        return;
    }
    if (n == WIFI_SCAN_FAILED || n < 0) {
        doc["running"] = false;
        doc["error"] = (int)n;
        send_json(200, doc);
        return;
    }
    doc["running"] = false;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 32; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["channel"] = WiFi.channel(i);
        o["secured"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    // Don't auto-delete; let the next /scan call replace.
    send_json(200, doc);
}

static void handle_wifi_connect() {
    if (!require_api_auth()) return;
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "json body required");
        return;
    }
    if (server.arg("plain").length() > 1024) {
        server.send(413, "text/plain", "body too large (1 KB max)");
        return;
    }
    JsonDocument doc(&yeyboats::psram_json);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "bad json");
        return;
    }
    const char *ssid = doc["ssid"];
    const char *pass = doc["password"] | "";
    if (!ssid || !*ssid) {
        server.send(400, "text/plain", "ssid required");
        return;
    }
    // Queue for the net worker (it'll save to NVS and reboot). Avoids
    // blocking the HTTP handler in the reboot delay path.
    app::Command cmd;
    cmd.type = app::CommandType::SaveWifi;
    strncpy(cmd.a, ssid, sizeof(cmd.a) - 1);
    strncpy(cmd.b, pass, sizeof(cmd.b) - 1);
    if (!app::post_net(cmd, 50)) {
        server.send(503, "text/plain", "net queue full");
        return;
    }
    JsonDocument out(&yeyboats::psram_json);
    out["queued"] = true;
    out["rebooting"] = true;
    out["ssid"] = ssid;
    send_json(202, out);
}

static void handle_wifi_forget() {
    if (!require_api_auth()) return;
    // Route through the net worker queue; wifi-forget reboots, which
    // would otherwise happen on the web task and cut off the response.
    app::Command cmd;
    cmd.type = app::CommandType::RunCommand;
    strncpy(cmd.a, "wifi-forget", sizeof(cmd.a) - 1);
    if (!app::post_net(cmd, 50)) {
        server.send(503, "text/plain", "net queue full");
        return;
    }
    JsonDocument out(&yeyboats::psram_json);
    out["queued"] = true;
    out["rebooting"] = true;
    send_json(202, out);
}

static void handle_wifi_saved_get() {
    if (!require_api_auth()) return;
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", wifi_store::to_json(false));
}

static void handle_wifi_saved_delete() {
    if (!require_api_auth()) return;
    // URI form: /api/wifi/saved/<ssid>
    String u = server.uri();
    int slash = u.lastIndexOf('/');
    if (slash < 0 || slash == (int)u.length() - 1) {
        server.send(400, "text/plain", "ssid required in path");
        return;
    }
    String ssid = u.substring(slash + 1);
    // URL-decode minimally (replace +)
    ssid.replace("+", " ");
    bool ok = wifi_store::remove(ssid.c_str());
    JsonDocument out(&yeyboats::psram_json);
    out["ok"] = ok;
    out["ssid"] = ssid;
    out["count"] = (uint32_t)wifi_store::count();
    send_json(ok ? 200 : 404, out);
}

// ---- /api/cmd ----------------------------------------------------------

static void handle_cmd() {
    if (!require_api_auth()) return;
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    String line = server.arg("plain");
    // Hard cap: command lines are routed through app::Command.a (256 B).
    // Reject oversized bodies before allocating the queue slot so a
    // hostile client can't flood the heap.
    if (line.length() > 512) {
        server.send(413, "text/plain", "command too long (512 B max)");
        return;
    }
    line.trim();
    if (line.length() == 0) {
        server.send(400, "text/plain", "empty command");
        return;
    }
    // Touch/gesture injection is intentionally NOT reachable over IP -
    // these commands run from BLE NUS / USB serial only. A network
    // attacker on the LAN should not be able to drive the UI.
    if (input_test::is_injection_command(line)) {
        server.send(403, "text/plain", "injection commands are BLE/serial only");
        return;
    }
    // Queue for the UI task. Many console commands touch LVGL state
    // (screen, theme, bright, demo, mob, ...) - executing them inline
    // here would race the LVGL render loop.
    app::Command cmd;
    cmd.type = app::CommandType::RunCommand;
    strncpy(cmd.a, line.c_str(), sizeof(cmd.a) - 1);
    if (!app::post(cmd, 50)) {
        server.send(503, "text/plain", "ui queue full");
        return;
    }
    JsonDocument doc(&yeyboats::psram_json);
    doc["queued"] = true;
    doc["cmd"] = line;
    send_json(202, doc);
}

// ---- /api/config -------------------------------------------------------
// Per docs/specs/08: surfaces the RAM-resident config + per-domain
// metadata. No NVS reads happen here - everything comes from
// config_runtime snapshots so this endpoint is cheap.

static void emit_meta(JsonObject &dst, ::config::Domain d) {
    ::config::DomainMeta m = ::config::meta(d);
    dst["schema"] = m.schema;
    dst["source"] = ::config::source_name(m.source);
    dst["revision"] = m.revision;
    dst["updated_ms"] = m.updated_ms;
    dst["dirty"] = m.dirty;
    dst["persist_pending"] = m.persist_pending;
    dst["persist_error"] = m.persist_error;
    if (m.last_error[0]) dst["last_error"] = m.last_error;
}

static void handle_config() {
    if (!require_api_auth()) return;
    ::config::UiConfig ui = ::config::ui();
    ::config::AlarmConfig al = ::config::alarms();
    ::config::SignalKConfig sk = ::config::signalk();

    JsonDocument doc(&yeyboats::psram_json);
    JsonObject root = doc.to<JsonObject>();

    JsonObject ui_obj = root["ui"].to<JsonObject>();
    JsonObject ui_v = ui_obj["values"].to<JsonObject>();
    ui_v["theme"] = ::config::theme_name(ui.theme);
    ui_v["brightness"] = ui.brightness;
    ui_v["pos_format"] = ::config::pos_format_name(ui.pos_format);
    ui_v["default_screen"] = ui.default_screen;
    JsonObject ui_m = ui_obj["meta"].to<JsonObject>();
    emit_meta(ui_m, ::config::Domain::Ui);

    JsonObject al_obj = root["alarms"].to<JsonObject>();
    JsonObject al_v = al_obj["values"].to<JsonObject>();
    al_v["depth_m"] = al.depth_alarm_m;
    al_v["battery_v"] = al.battery_alarm_v;
    al_v["audible"] = al.audible;
    JsonObject al_m = al_obj["meta"].to<JsonObject>();
    emit_meta(al_m, ::config::Domain::Alarms);

    JsonObject sk_obj = root["signalk"].to<JsonObject>();
    JsonObject sk_v = sk_obj["values"].to<JsonObject>();
    sk_v["host"] = sk.host;
    sk_v["port"] = sk.port;
    // Token is sensitive - report presence only.
    sk_v["has_token"] = (sk.token[0] != 0);
    JsonObject sk_m = sk_obj["meta"].to<JsonObject>();
    emit_meta(sk_m, ::config::Domain::SignalK);

    send_json(200, doc);
}

static void handle_config_status() {
    if (!require_api_auth()) return;
    JsonDocument doc(&yeyboats::psram_json);
    JsonObject root = doc.to<JsonObject>();
    root["jobs_queued"] = ::config::persist_jobs_queued();
    root["jobs_completed"] = ::config::persist_jobs_completed();
    root["jobs_failed"] = ::config::persist_jobs_failed();
    root["coalesced"] = ::config::coalesced_writes();
    JsonObject domains = root["domains"].to<JsonObject>();
    JsonObject u = domains["ui"].to<JsonObject>();
    emit_meta(u, ::config::Domain::Ui);
    JsonObject a = domains["alarms"].to<JsonObject>();
    emit_meta(a, ::config::Domain::Alarms);
    JsonObject k = domains["signalk"].to<JsonObject>();
    emit_meta(k, ::config::Domain::SignalK);
    send_json(200, doc);
}

// ---- /api/screenshot.png -----------------------------------------------
// PNG-encoded LVGL snapshot. The encoder lives in screenshot.cpp using
// ROM miniz; output is typically 20-60 kB for a 480x480 UI screen,
// small enough to fit in a single TCP send window (so the chunked-
// streaming wedge that breaks the .bmp endpoint never triggers).

static void handle_screenshot_png() {
    if (!require_api_auth()) return;
    if (!heap_ok(HEAP_MEDIUM)) return;
    static volatile bool s_in_flight = false;
    if (s_in_flight) {
        server.send(429, "text/plain", "screenshot busy");
        return;
    }
    s_in_flight = true;
    uint8_t *png = nullptr;
    size_t len = 0;
    bool ok = screenshot::request(5000, &png, &len, screenshot::Format::Png);
    if (!ok || !png || len == 0) {
        s_in_flight = false;
        server.send(504, "text/plain", "snapshot timeout");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    // Use the String(uint8_t*, len) constructor which preserves NULs
    // and bundles everything into a single server.send() call. That's
    // the only path on Arduino-ESP32 WebServer that reliably delivers
    // a body - sendContent / send_P / direct WiFiClient.write all
    // dropped bytes silently (~4 kB through, then nothing) with this
    // version of the library.
    String body((const uint8_t *)png, len);
    // Diag: try application/octet-stream to rule out content-type-based
    // filtering in WebServer or its TLS/middleware path. Image/png with
    // identical body went out as HTTP 200 + Content-Length but with
    // 0 bytes received downstream.
    server.send(200, "application/octet-stream", body);
    heap_caps_free(png);
    s_in_flight = false;
}

// ---- /api/screenshot.bmp -----------------------------------------------
// LVGL snapshot of the active screen. Requires LV_USE_SNAPSHOT in lv_conf.h.

static void handle_screenshot() {
    if (!require_api_auth()) return;
    // Screenshot is the most heap-hostile endpoint we serve (~460 kB
    // PSRAM BMP + sustained socket writes that block the WiFi task).
    // Guard with the heap breaker first.
    if (!heap_ok(HEAP_HEAVY)) return;
    // In-flight guard: only one screenshot at a time. A second request
    // landing while we're still chunking would starve WiFi twice and
    // confuse serve_pending().
    static volatile bool s_in_flight = false;
    if (s_in_flight) {
        server.send(429, "text/plain", "screenshot busy");
        return;
    }
    s_in_flight = true;

    uint8_t *bmp = nullptr;
    size_t len = 0;
    bool ok = screenshot::request(2500, &bmp, &len);
    if (!ok || !bmp || len == 0) {
        s_in_flight = false;
        server.send(504, "text/plain", "snapshot timeout");
        return;
    }
    // Chunked write with a per-write timeout AND a periodic yield so
    // the WiFi task gets CPU. Without the yield, blasting ~460 kB on
    // this task starves WiFi keepalives and the AP can drop the
    // station mid-transfer. This still has issues streaming the full
    // 460 kB through Arduino-ESP32 WebServer's chunked path (see the
    // E2E test in tools/test-screenshot.sh) but a 503/timeout is
    // safer than the prior wedged-WiFi state.
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(len);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    client.setTimeout(2000);
    const uint32_t deadline = millis() + 8000;
    const uint8_t *p = bmp;
    size_t left = len;
    while (left && client.connected()) {
        if ((int32_t)(millis() - deadline) > 0) break;
        size_t chunk = left > 1460 ? 1460 : left;
        size_t w = client.write(p, chunk);
        if (w == 0) break;
        p += w;
        left -= w;
        if ((((size_t)(p - bmp)) & 0x3FFF) == 0) vTaskDelay(1);
    }
    heap_caps_free(bmp);
    s_in_flight = false;
}

static void handle_logs() {
    if (!require_api_auth()) return;
    // /api/logs serializes the entire requested slice as one JSON
    // payload; a `limit=96` response is ~19 kB and serializeJson grows
    // its String buffer on internal heap. Apply the heap breaker so a
    // dashboard polling logs doesn't tip the device over the cliff.
    if (!heap_ok(HEAP_MEDIUM)) return;
    uint32_t since = 0;
    if (server.hasArg("since")) since = (uint32_t)strtoul(server.arg("since").c_str(), nullptr, 10);
    // `?limit=N` caps how many entries we return in one response. Default
    // is 32 (~6 KiB JSON, fits comfortably in an lwIP TCP send window) so
    // polling clients keep each request cheap and the lwIP socket pool
    // recycles without backing up. Pages via the `?since=<lastSeq>` reply
    // field; absolute max stays at the ring size (96) for clients that
    // explicitly ask. Stress-testing /api/logs?since=0 in a tight loop
    // exhausted TIME_WAIT sockets at 12+ req/s when the full ring was
    // returned every call - smaller pages avoid that.
    constexpr size_t kRingCap = 96;
    size_t limit = 32;
    if (server.hasArg("limit")) {
        unsigned long lv = strtoul(server.arg("limit").c_str(), nullptr, 10);
        if (lv == 0) lv = 32;
        if (lv > kRingCap) lv = kRingCap;
        limit = (size_t)lv;
    }
    // The full ring is ~19 KiB (96 * 200 B). The web task has an 8 KiB
    // stack and stack-canary checks are off in sdkconfig, so allocating
    // this on the stack silently corrupts whatever sits above it and
    // panics the device the next time that memory is touched. Move to
    // PSRAM (plenty of room, slower access is fine for a debug endpoint
    // that runs at human-poll rates).
    net::LogEntry *entries =
        (net::LogEntry *)heap_caps_malloc(sizeof(net::LogEntry) * limit, MALLOC_CAP_SPIRAM);
    if (!entries) {
        // Fall back to internal heap if PSRAM is exhausted (unlikely);
        // even the full 19 KiB is within the ~150 KiB free internal heap
        // budget this firmware runs with.
        entries = (net::LogEntry *)malloc(sizeof(net::LogEntry) * limit);
    }
    if (!entries) {
        server.send(503, "text/plain", "no memory for log buffer");
        return;
    }
    size_t n = net::copyLogs(entries, limit, since);

    JsonDocument doc(&yeyboats::psram_json);
    JsonArray arr = doc["entries"].to<JsonArray>();
    uint32_t last = since;
    for (size_t i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["seq"] = entries[i].seq;
        o["ms"] = entries[i].ms;
        o["line"] = entries[i].line;
        last = entries[i].seq;
    }
    doc["lastSeq"] = last;
    doc["truncated"] = n == limit;
    // serialize FIRST, then free. ArduinoJson v7 stores `const char *`
    // values as pointers (zero-copy), so the entries buffer must stay
    // alive until serializeJson() (inside send_json) finishes reading
    // it. The earlier version of this fix freed before send_json and
    // serialized dangling pointers - manifested as /api/logs hangs
    // while other endpoints still worked.
    send_json(200, doc);
    free(entries);
}

// ---- root HTML page ----------------------------------------------------
// Self-contained vanilla HTML+JS. No external CDN. All DOM writes go
// through textContent or createElement/append - no innerHTML with values.

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Yey Boats Instruments</title>
<style>
  body{font:14px system-ui,sans-serif;background:#05101c;color:#eaf2ff;margin:0;padding:16px}
  h1{margin:0 0 4px 0;font-size:18px;color:#9ec5fe}
  .row{display:flex;flex-wrap:wrap;gap:12px;margin-top:12px}
  .card{background:#0a2540;border:1px solid #223a55;border-radius:8px;padding:12px;min-width:240px;flex:1}
  .k{color:#6c8bb1;font-size:12px;letter-spacing:.05em}
  .v{font-size:18px;margin-bottom:6px}
  button{background:#3b6294;color:white;border:0;padding:6px 12px;border-radius:6px;cursor:pointer;margin:2px;font:inherit}
  button.active{background:#9ec5fe;color:#0a1a2b;font-weight:600}
  textarea{width:100%;height:200px;background:#05101c;color:#eaf2ff;border:1px solid #223a55;border-radius:6px;padding:8px;font-family:monospace;font-size:12px;box-sizing:border-box}
  pre{background:#05101c;border:1px solid #223a55;border-radius:6px;padding:8px;overflow:auto;max-height:180px;font-size:12px;margin:0}
  #liveLog{max-height:320px;white-space:pre-wrap}
  input{background:#05101c;color:#eaf2ff;border:1px solid #223a55;border-radius:4px;padding:4px 8px;font-family:monospace}
  .status{color:#33d17a}
  .status.stalled{color:#ffb84d}
  .status.disconnected{color:#ff4d6d}
</style>
<h1>Yey Boats Instruments</h1>
<div class=row>
  <div class=card>
    <div class=k>DEVICE</div><div class=v id=dev>-</div>
    <div class=k>WIFI</div><div class=v id=wifi>-</div>
    <div class=k>SIGNALK</div><div class=v><span id=sk class=status>-</span></div>
    <div class=k>SCREEN</div><div class=v id=screen>-</div>
  </div>
  <div class=card>
    <div class=k>SCREENS</div>
    <div id=screens></div>
    <div class=k style="margin-top:8px">THEME</div>
    <button data-cmd="theme day">day</button>
    <button data-cmd="theme night">night</button>
    <div class=k style="margin-top:8px">BRIGHTNESS</div>
    <input type=range id=brightSlider min=20 max=255 value=200 style="width:100%">
  </div>
  <div class=card>
    <div class=k>COMMAND  (<a href="/help/commands" style="color:#9ec5fe">catalog</a>)</div>
    <input id=cmd placeholder="e.g. sk-status"/>
    <button id=cmdSend>send</button>
    <pre id=cmdOut></pre>
  </div>
</div>
<div class=row>
  <div class=card style="flex:1 0 100%">
    <div class=k>LIVE DEVICE LOG</div>
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:4px 0 8px 0">
      <button id=logPause>pause</button>
      <button id=logClear>clear view</button>
      <span id=logState class=k>connecting</span>
    </div>
    <pre id=liveLog></pre>
  </div>
</div>
<div class=row>
  <div class=card style="flex:1 0 100%">
    <div class=k>WIFI</div>
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px">
      <button id=wifiScan>scan</button>
      <span id=wifiStatus class=k>idle</span>
      <input id=wifiSsid placeholder="ssid" style="width:160px"/>
      <input id=wifiPass placeholder="password (blank if open)" type=password style="width:220px"/>
      <button id=wifiConnect>save + reboot</button>
      <button id=wifiForget>forget all</button>
    </div>
    <div class=k>SAVED NETWORKS  (tried in order on boot)</div>
    <div id=wifiSaved style="margin-bottom:8px"></div>
    <div class=k>NEARBY</div>
    <div id=wifiList></div>
  </div>
</div>
<div class=row>
  <div class=card style="flex:1 0 100%">
    <div class=k>DASHBOARD CONFIG</div>
    <div style="margin:4px 0 8px 0">
      <button id=dashboardJson>export json</button>
      <button id=dashboardYaml>export yaml</button>
      <span class=k>YAML on-device uses JSON-compatible YAML syntax.</span>
    </div>
    <textarea id=layout></textarea>
    <button id=layoutApply>import/apply</button>
    <button id=layoutLoad>refresh</button>
    <span id=layoutMsg></span>
  </div>
</div>

<script>
async function refresh(){
  try{
    const s = await (await fetch('/api/state')).json();
    document.getElementById('dev').textContent = s.device.id + '  build ' + s.device.build + '  heap ' + Math.round(s.device.heap_free/1024) + ' kB  psram ' + Math.round(s.device.psram_free/1024) + ' kB';
    document.getElementById('wifi').textContent = s.wifi.mode + '  ' + (s.wifi.ssid||'-') + '  ' + s.wifi.ip + '  ' + (s.wifi.rssi||0) + ' dBm';
    const sk = document.getElementById('sk');
    sk.textContent = s.sk.state + '  ' + s.sk.host + ':' + s.sk.port;
    sk.className = 'status ' + s.sk.state;
    document.getElementById('screen').textContent = s.screen.title + ' (' + (s.screen.index+1) + '/' + s.screen.count + ')';

    const screens = await (await fetch('/api/screens')).json();
    const sc = document.getElementById('screens');
    sc.replaceChildren();
    for (const t of screens) {
      const b = document.createElement('button');
      b.textContent = t.title + (t.hidden ? ' .' : '');
      if (t.active) b.className = 'active';
      b.addEventListener('click', () => fetch('/api/screen/' + encodeURIComponent(t.id), {method:'POST'}).then(refresh));
      sc.appendChild(b);
    }
  }catch(e){ /* swallow transient errors during reload */ }
}
async function loadLayout(){
  try{
    const r = await fetch('/api/dashboard/config.json');
    if (!r.ok) { document.getElementById('layoutMsg').textContent = 'no layout'; return; }
    const t = await r.text();
    document.getElementById('layout').value = JSON.stringify(JSON.parse(t), null, 2);
    document.getElementById('layoutMsg').textContent = '';
  }catch(e){ document.getElementById('layoutMsg').textContent = String(e.message||e); }
}
async function saveLayout(){
  const t = document.getElementById('layout').value;
  const r = await fetch('/api/dashboard/config.json', {method:'PUT', headers:{'Content-Type':'application/json'}, body:t});
  document.getElementById('layoutMsg').textContent = (r.ok?'ok ':'fail ') + r.status;
}
async function exportDashboard(kind){
  const url = kind === 'yaml' ? '/api/dashboard/config.yaml' : '/api/dashboard/config.json';
  const r = await fetch(url);
  document.getElementById('layout').value = await r.text();
  document.getElementById('layoutMsg').textContent = kind + ' export loaded';
}
async function runCmd(){
  const c = document.getElementById('cmd').value;
  const r = await fetch('/api/cmd', {method:'POST', headers:{'Content-Type':'text/plain'}, body:c});
  document.getElementById('cmdOut').textContent = await r.text();
  refresh();
}
async function sendCmd(c){
  await fetch('/api/cmd', {method:'POST', headers:{'Content-Type':'text/plain'}, body:c});
  refresh();
}
document.getElementById('cmdSend').addEventListener('click', runCmd);
document.getElementById('cmd').addEventListener('keydown', e => { if (e.key === 'Enter') runCmd(); });
document.getElementById('layoutApply').addEventListener('click', saveLayout);
document.getElementById('layoutLoad').addEventListener('click', loadLayout);
document.getElementById('dashboardJson').addEventListener('click', () => exportDashboard('json'));
document.getElementById('dashboardYaml').addEventListener('click', () => exportDashboard('yaml'));
for (const b of document.querySelectorAll('button[data-cmd]')) {
  b.addEventListener('click', () => sendCmd(b.getAttribute('data-cmd')));
}

let wifiPoll = null;
let logSince = 0;
let logPaused = false;
const logLines = [];
function appendLogLine(entry){
  const t = String(Math.floor(entry.ms / 1000)).padStart(6, ' ');
  logLines.push(t + 's #' + entry.seq + ' ' + entry.line);
  while (logLines.length > 240) logLines.shift();
  const box = document.getElementById('liveLog');
  const nearBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 16;
  box.textContent = logLines.join('\n');
  if (nearBottom) box.scrollTop = box.scrollHeight;
}
async function refreshLogs(){
  if (logPaused) return;
  try{
    const r = await fetch('/api/logs?since=' + encodeURIComponent(logSince));
    const j = await r.json();
    for (const entry of (j.entries || [])) {
      appendLogLine(entry);
      logSince = Math.max(logSince, entry.seq);
    }
    document.getElementById('logState').textContent = 'live  last #' + logSince;
  }catch(e){
    document.getElementById('logState').textContent = 'offline';
  }
}
async function wifiScan(){
  await fetch('/api/wifi/scan', {method:'POST'});
  document.getElementById('wifiStatus').textContent = 'scanning...';
  if (wifiPoll) clearInterval(wifiPoll);
  wifiPoll = setInterval(wifiPollOnce, 1500);
}
async function wifiPollOnce(){
  try{
    const r = await fetch('/api/wifi/networks');
    const j = await r.json();
    if (j.running) {
      document.getElementById('wifiStatus').textContent = 'scanning...';
      return;
    }
    if (wifiPoll) { clearInterval(wifiPoll); wifiPoll = null; }
    const list = document.getElementById('wifiList');
    list.replaceChildren();
    document.getElementById('wifiStatus').textContent = (j.networks ? j.networks.length : 0) + ' networks';
    for (const n of (j.networks || [])) {
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #1a2a3a;cursor:pointer';
      const left = document.createElement('span');
      left.textContent = n.ssid + (n.secured ? '  [lock]' : '  [open]');
      const right = document.createElement('span');
      right.className = 'k';
      right.textContent = n.rssi + ' dBm  ch ' + n.channel;
      row.appendChild(left); row.appendChild(right);
      row.addEventListener('click', () => {
        document.getElementById('wifiSsid').value = n.ssid;
        document.getElementById('wifiPass').focus();
      });
      list.appendChild(row);
    }
  }catch(e){ /* transient */ }
}
async function wifiConnect(){
  const ssid = document.getElementById('wifiSsid').value;
  const password = document.getElementById('wifiPass').value;
  if (!ssid) { document.getElementById('wifiStatus').textContent = 'enter ssid'; return; }
  document.getElementById('wifiStatus').textContent = 'saving + rebooting...';
  await fetch('/api/wifi/connect', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ssid, password})});
}
async function wifiForget(){
  document.getElementById('wifiStatus').textContent = 'forgetting + rebooting...';
  await fetch('/api/wifi/forget', {method:'POST'});
}
async function wifiSavedRefresh(){
  try{
    const r = await fetch('/api/wifi/saved');
    const arr = await r.json();
    const box = document.getElementById('wifiSaved');
    box.replaceChildren();
    if (!arr.length) {
      const e = document.createElement('span'); e.className = 'k'; e.textContent = 'none';
      box.appendChild(e); return;
    }
    for (let i = 0; i < arr.length; ++i) {
      const n = arr[i];
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;justify-content:space-between;padding:3px 0;border-bottom:1px solid #1a2a3a';
      const left = document.createElement('span');
      left.textContent = (i+1) + '. ' + n.ssid + (n.has_password ? '  [+pw]' : '  [open]');
      const del = document.createElement('button');
      del.textContent = 'forget';
      del.addEventListener('click', async () => {
        await fetch('/api/wifi/saved/' + encodeURIComponent(n.ssid).replace(/%20/g,'+'), {method:'DELETE'});
        wifiSavedRefresh();
      });
      row.appendChild(left); row.appendChild(del);
      box.appendChild(row);
    }
  }catch(e){ /* transient */ }
}

document.getElementById('wifiScan').addEventListener('click', wifiScan);
document.getElementById('wifiConnect').addEventListener('click', wifiConnect);
document.getElementById('wifiForget').addEventListener('click', wifiForget);
document.getElementById('logPause').addEventListener('click', () => {
  logPaused = !logPaused;
  document.getElementById('logPause').textContent = logPaused ? 'resume' : 'pause';
  document.getElementById('logState').textContent = logPaused ? 'paused' : 'live';
});
document.getElementById('logClear').addEventListener('click', () => {
  logLines.length = 0;
  document.getElementById('liveLog').textContent = '';
});

let brightT = null;
document.getElementById('brightSlider').addEventListener('input', e => {
  if (brightT) clearTimeout(brightT);
  brightT = setTimeout(() => sendCmd('bright ' + e.target.value), 120);
});

refresh();
loadLayout();
wifiSavedRefresh();
refreshLogs();
setInterval(refresh, 5000);
setInterval(refreshLogs, 1000);
</script>
)HTML";

static void handle_root() {
    if (!require_api_auth()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", FPSTR(INDEX_HTML));
}

// ---- setup / loop ------------------------------------------------------

// In captive mode, ensure we also explicitly answer the well-known probe
// paths (some clients require an exact route match, not catch-all).
static void handle_probe() {
    if (captive_active) {
        send_captive_page("probe-explicit");
    } else {
        // STA mode: be nice and return the OS-expected no-captive response
        // so devices proxying through us don't mis-detect a captive portal.
        String u = server.uri();
        if (u == "/generate_204" || u == "/gen_204") {
            server.send(204);
        } else if (u == "/hotspot-detect.html") {
            server.send(200, "text/html",
                        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        } else {
            server.send(200, "text/plain", "Microsoft NCSI");
        }
    }
}

// ---- /api/p2p/* : espdisp control protocol (target side) ----------------
// Versioned HTTP/JSON binding of the control protocol. POST bodies are parsed
// with ArduinoJson -> proto::from_json, gated by version_compatible, handled by
// proto_target (auth + session table + UI-task view switch), and the ack is
// serialized back. GETs reflect the device record / control state. The shared
// key inside the message gates control; require_api_auth() is intentionally not
// applied here so peer controllers can attach without the web admin token.

static bool p2p_read_body(JsonDocument &doc) {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"empty_body\"}");
        return false;
    }
    const String &body = server.arg("plain");
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"error\":\"bad_json\"}");
        return false;
    }
    return true;
}

template <typename Ack> static void p2p_send_ack(const Ack &ack) {
    JsonDocument out(&yeyboats::psram_json);
    proto::to_json(out.to<JsonObject>(), ack);
    String payload;
    serializeJson(out, payload);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", payload);
}

static void handle_p2p_device() {
    JsonDocument out(&yeyboats::psram_json);
    // DeviceRecord is ~1.5 KB; keep it off the web task stack. Handlers run
    // serially on the single WebServer task, so a function-static is race-free.
    static proto::DeviceRecord r;
    memset(&r, 0, sizeof(r));
    proto_target::fill_device_record(r);
    proto::to_json(out.to<JsonObject>(), r);
    String payload;
    serializeJson(out, payload);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", payload);
}

static void handle_p2p_attach() {
    JsonDocument doc(&yeyboats::psram_json);
    if (!p2p_read_body(doc)) return;
    proto::Attach req;
    proto::from_json(doc.as<JsonObjectConst>(), req);
    if (!proto::version_compatible(req.v)) {
        server.send(400, "application/json", "{\"error\":\"incompatible_version\"}");
        return;
    }
    // AttachAck embeds a DeviceRecord (~1.5 KB) — static to keep it off the web
    // task stack (serial handler execution makes the static safe).
    static proto::AttachAck ack;
    memset(&ack, 0, sizeof(ack));
    proto_target::handle_attach(req, ack);
    p2p_send_ack(ack);
}

static void handle_p2p_switch() {
    JsonDocument doc(&yeyboats::psram_json);
    if (!p2p_read_body(doc)) return;
    proto::Switch req;
    proto::from_json(doc.as<JsonObjectConst>(), req);
    if (!proto::version_compatible(req.v)) {
        server.send(400, "application/json", "{\"error\":\"incompatible_version\"}");
        return;
    }
    proto::SwitchAck ack;
    proto_target::handle_switch(req, ack);
    p2p_send_ack(ack);
}

static void handle_p2p_heartbeat() {
    JsonDocument doc(&yeyboats::psram_json);
    if (!p2p_read_body(doc)) return;
    proto::Heartbeat req;
    proto::from_json(doc.as<JsonObjectConst>(), req);
    if (!proto::version_compatible(req.v)) {
        server.send(400, "application/json", "{\"error\":\"incompatible_version\"}");
        return;
    }
    bool ok = proto_target::handle_heartbeat(req.sessionId);
    proto::HeartbeatAck ack;
    strncpy(ack.v, "1.0", sizeof(ack.v) - 1);
    ack.ok = ok;
    ack.ttlMs = proto::kDefaultTtlMs;
    p2p_send_ack(ack);
}

static void handle_p2p_detach() {
    JsonDocument doc(&yeyboats::psram_json);
    if (!p2p_read_body(doc)) return;
    proto::Detach req;
    proto::from_json(doc.as<JsonObjectConst>(), req);
    if (!proto::version_compatible(req.v)) {
        server.send(400, "application/json", "{\"error\":\"incompatible_version\"}");
        return;
    }
    bool ok = proto_target::handle_detach(req.sessionId);
    JsonDocument out(&yeyboats::psram_json);
    JsonObject o = out.to<JsonObject>();
    o["v"] = "1.0";
    o["t"] = "detachAck";
    o["ok"] = ok;
    String payload;
    serializeJson(out, payload);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", payload);
}

static void handle_p2p_state() {
    JsonDocument out(&yeyboats::psram_json);
    // ControlState is ~2 KB (Session[16]); static keeps it off the web stack.
    static proto::ControlState cs;
    memset(&cs, 0, sizeof(cs));
    proto_target::fill_state(cs);
    proto::to_json(out.to<JsonObject>(), cs);
    String payload;
    serializeJson(out, payload);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", payload);
}

static void bind_routes() {
    server.on("/", HTTP_GET, handle_root);

    // Captive-portal probe URLs (must match exactly on some OSes).
    server.on("/generate_204", HTTP_GET, handle_probe);
    server.on("/gen_204", HTTP_GET, handle_probe);
    server.on("/hotspot-detect.html", HTTP_GET, handle_probe);
    server.on("/library/test/success.html", HTTP_GET, handle_probe);
    server.on("/connecttest.txt", HTTP_GET, handle_probe);
    server.on("/ncsi.txt", HTTP_GET, handle_probe);
    server.on("/success.txt", HTTP_GET, handle_probe);
    server.on("/redirect", HTTP_GET, handle_probe);
    server.on("/chat", HTTP_GET, handle_probe);
    server.on("/check_network_status.txt", HTTP_GET, handle_probe);
    server.on("/api/state", HTTP_GET, handle_state);
    server.on("/api/diag", HTTP_GET, handle_diag);
    server.on("/api/config", HTTP_GET, handle_config);
    server.on("/api/config/status", HTTP_GET, handle_config_status);
    server.on("/api/security", HTTP_GET, handle_security);
    server.on("/api/screens", HTTP_GET, handle_screens);
    server.on("/api/p2p/device", HTTP_GET, handle_p2p_device);
    server.on("/api/p2p/attach", HTTP_POST, handle_p2p_attach);
    server.on("/api/p2p/switch", HTTP_POST, handle_p2p_switch);
    server.on("/api/p2p/heartbeat", HTTP_POST, handle_p2p_heartbeat);
    server.on("/api/p2p/detach", HTTP_POST, handle_p2p_detach);
    server.on("/api/p2p/state", HTTP_GET, handle_p2p_state);
    server.on("/api/sk", HTTP_GET, handle_sk_data);
    server.on("/api/boat", HTTP_GET, handle_boat);
    server.on("/api/logs", HTTP_GET, handle_logs);
    server.on("/api/commands", HTTP_GET, handle_commands_json);
    server.on("/help/commands", HTTP_GET, handle_commands_html);
    server.on("/api/layout", HTTP_GET, handle_layout_get);
    server.on("/api/layout", HTTP_PUT, handle_layout_put);
    server.on("/api/midl/manifest", HTTP_GET, handle_midl_manifest);
    server.on("/api/midl/config", HTTP_POST, handle_midl_config_post);
    server.on("/api/midl/config", HTTP_GET, handle_midl_config_get);
    server.on("/api/midl/reset", HTTP_POST, handle_midl_reset);
    server.on("/api/dashboard/config.json", HTTP_GET, handle_dashboard_config_get_json);
    server.on("/api/dashboard/config.json", HTTP_PUT, handle_dashboard_config_put);
    server.on("/api/dashboard/config.yaml", HTTP_GET, handle_dashboard_config_get_yaml);
    server.on("/api/dashboard/config.yaml", HTTP_PUT, handle_dashboard_config_put);
    server.on("/api/cmd", HTTP_POST, handle_cmd);
    server.on("/api/wifi/scan", HTTP_POST, handle_wifi_scan);
    server.on("/api/wifi/networks", HTTP_GET, handle_wifi_networks);
    server.on("/api/wifi/connect", HTTP_POST, handle_wifi_connect);
    server.on("/api/wifi/forget", HTTP_POST, handle_wifi_forget);
    server.on("/api/wifi/saved", HTTP_GET, handle_wifi_saved_get);
    server.on("/api/screenshot.bmp", HTTP_GET, handle_screenshot);
    server.on("/api/screenshot.png", HTTP_GET, handle_screenshot_png);
    server.onNotFound([]() {
        if (server.method() == HTTP_POST && server.uri().startsWith("/api/screen/")) {
            handle_screen_set();
            return;
        }
        if (server.method() == HTTP_DELETE && server.uri().startsWith("/api/wifi/saved/")) {
            handle_wifi_saved_delete();
            return;
        }
        if (server.method() == HTTP_OPTIONS) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
            server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");
            server.send(204);
            return;
        }
        // Captive portal: in AP mode, serve the config page inline for
        // any unknown request. Captive-portal browsers tend to render
        // whatever the probe returns rather than follow a 302 the way a
        // normal browser would, so we skip the redirect step entirely.
        if (captive_active) {
            IPAddress ap_ip = WiFi.softAPIP();
            String host = server.hostHeader();
            host.toLowerCase();
            String ap = ap_ip.toString();
            bool foreign_host = host.length() > 0 && host != ap;
            if (is_captive_probe_path(server.uri()) || foreign_host) {
                send_captive_page(foreign_host ? "foreign-host" : "probe-path");
                return;
            }
        }
        server.send(404, "text/plain", "not found");
    });
}

// ---- task --------------------------------------------------------------
// WebServer is fully synchronous - if we call handleClient() from the same
// task as LVGL, a slow socket flush blocks UI rendering. Run the server on
// its own FreeRTOS task pinned to core 0 (Arduino loop runs on core 1).
// All handlers above touch only thread-safe accessors (no direct LVGL
// drawing). ui::show / layout::apply_json toggle flags / PSRAM buffers
// that the LVGL render loop reads atomically enough for our use - worst
// case is one frame of mismatched state, never a crash.

static TaskHandle_t s_task = nullptr;

static void sync_captive_dns() {
    net::WifiState state = net::wifiState();
    if (state != s_bound_state &&
        (state == net::WifiState::StaUp || state == net::WifiState::ApSetup)) {
        // Bind once the actual STA/AP interface is up. Calling
        // server.begin() before lwIP's TCPIP task is initialized
        // (i.e., before WiFi.mode/begin) trips "Invalid mbox" in
        // tcpip_send_msg_wait_sem and panics the device.
        server.begin();
        bool first = (s_bound_state == net::WifiState::Idle);
        s_bound_state = state;
        net::logf("[web] http %s on :80 for wifi=%s", first ? "bound" : "rebound",
                  net::wifiStateName());
    }

    bool want_captive = (state == net::WifiState::ApSetup);
    if (want_captive == captive_active) return;

    if (want_captive) {
        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(53, "*", WiFi.softAPIP());
        captive_active = true;
        net::logf("[web] captive DNS on :53 -> %s", WiFi.softAPIP().toString().c_str());
    } else {
        if (captive_active) {
            dns.stop();
            net::logf("[web] captive DNS stopped");
        }
        captive_active = false;
    }
}

static void web_task(void *) {
    // Defer server.begin() until sync_captive_dns() sees WiFi reach
    // StaUp or ApSetup - by then lwIP's TCPIP task is up. Calling
    // begin() here while state is still Idle asserts inside lwIP.
    net::logf("[web] http task on core %d (deferring bind until wifi up)", xPortGetCoreID());
    for (;;) {
        sync_captive_dns();
        if (captive_active) dns.processNextRequest();
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void setup() {
    if (started) return;
    proto_target::setup();
    bind_routes();
    BaseType_t r = xTaskCreatePinnedToCore(web_task, "web", 8192, nullptr, 1 /* low prio */,
                                           &s_task, 0 /* core 0 */);
    if (r != pdPASS) {
        net::logf("[web] task create failed");
        return;
    }
    started = true;
}

void loop() {
    // Server runs on its own task - nothing to do here.
}

}  // namespace web
