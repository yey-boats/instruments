#include "web.h"

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>

#include "net.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ui_screens.h"
#include "board_pins.h"
#include "wifi_store.h"
#include "boat_data.h"
#include "source_nmea_wifi.h"
#include "source_nmea2000.h"
#include "input_test.h"
#include "cmd_catalog.h"
#include "ui_data.h"
#include "screenshot.h"
#include "app_events.h"
#include "config_runtime.h"
#include <esp_heap_caps.h>

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
    return p == "/generate_204" || p == "/gen_204" ||
           p == "/hotspot-detect.html" || p == "/library/test/success.html" ||
           p == "/connecttest.txt" || p == "/ncsi.txt" ||
           p == "/success.txt" || p == "/redirect" ||
           p == "/chat" || p == "/check_network_status.txt";
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

// ---- /api/state --------------------------------------------------------

static void handle_state() {
    JsonDocument doc;
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["id"] = net::deviceId();
    dev["uptime_ms"] = (uint32_t)millis();
    dev["heap_free"] = (uint32_t)ESP.getFreeHeap();
    dev["psram_free"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    dev["build"] = __DATE__ " " __TIME__;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["up"] = net::wifiUp();
    wifi["mode"] = net::wifiUp() ? "STA" : "AP";
    wifi["state"] = net::wifiStateName();
    wifi["ssid"] = WiFi.SSID();
    wifi["ip"] = net::ipString();
    wifi["rssi"] = net::rssi();

    JsonObject sk = doc["sk"].to<JsonObject>();
    sk["state"] = sk::connectionStatus();
    Preferences p;
    p.begin("sk", true);
    sk["host"] = p.getString("host", "");
    sk["port"] = p.getUInt("port", 3000);
    // Never echo the token itself - only whether one is configured.
    // CLAUDE.md: "the SignalK token field is sensitive."
    sk["has_token"] = p.getString("token", "").length() > 0;
    p.end();
    // Task diagnostics: confirms sk_task is alive (iters keeps growing)
    // and exposes the peak ws.loop() duration since last sample. A high
    // peak here is fine - it's on core 0, not on the LVGL task.
    sk["task_iters"] = sk::loopIters();
    sk["task_peak_us"] = sk::loopMaxUs();

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
        Preferences pu;
        pu.begin("ui", true);
        ui_o["theme"] = pu.getString("theme", "night");
        pu.end();
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
    JsonDocument doc;
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
    JsonDocument doc;
    doc["queued"] = true;
    doc["target"] = id;
    send_json(202, doc);
}

// ---- /api/sk -----------------------------------------------------------

static void put_double(JsonObject o, const char *k, double v) {
    if (!isnan(v)) o[k] = v;
}

static void handle_sk_data() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    JsonDocument doc;
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
    doc["lastUpdateAgeMs"] =
        d.lastUpdateMs ? (uint32_t)(millis() - d.lastUpdateMs) : (uint32_t)0;

    send_json(200, doc);
}

// ---- /api/boat ---------------------------------------------------------
// Per-field source / age / freshness view onto boat::Snapshot. Tests
// use this to verify that priority + freshness routes the right
// source-of-truth to renderers.

static void handle_boat() {
    boat::Snapshot s;
    boat::copy_snapshot(s);
    boat::Timeouts t = boat::get_timeouts();
    boat::Priority p = boat::get_priority();
    uint32_t now = millis();

    JsonDocument doc;
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
    JsonDocument doc;
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
              "<title>esp32-boat-mfd - commands</title>\n"
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
              "<h1>esp32-boat-mfd commands</h1>\n"
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

// ---- /api/layout (GET / PUT) -------------------------------------------

static void handle_layout_get() {
    String body;
    if (!layout::copy_last_json(body)) {
        server.send(404, "text/plain", "no layout loaded");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", body);
}

static void handle_layout_put() {
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
    JsonDocument doc;
    doc["queued"] = true;
    doc["size"] = (uint32_t)len;
    send_json(202, doc);
}

// ---- /api/wifi/scan, /networks, /connect -------------------------------

static bool s_scan_started = false;

static void handle_wifi_scan() {
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
    int n = WiFi.scanComplete();
    JsonDocument doc;
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
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "json body required");
        return;
    }
    JsonDocument doc;
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
    JsonDocument out;
    out["queued"] = true;
    out["rebooting"] = true;
    out["ssid"] = ssid;
    send_json(202, out);
}

static void handle_wifi_forget() {
    // Route through the net worker queue; wifi-forget reboots, which
    // would otherwise happen on the web task and cut off the response.
    app::Command cmd;
    cmd.type = app::CommandType::RunCommand;
    strncpy(cmd.a, "wifi-forget", sizeof(cmd.a) - 1);
    if (!app::post_net(cmd, 50)) {
        server.send(503, "text/plain", "net queue full");
        return;
    }
    JsonDocument out;
    out["queued"] = true;
    out["rebooting"] = true;
    send_json(202, out);
}

static void handle_wifi_saved_get() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", wifi_store::to_json(false));
}

static void handle_wifi_saved_delete() {
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
    JsonDocument out;
    out["ok"] = ok;
    out["ssid"] = ssid;
    out["count"] = (uint32_t)wifi_store::count();
    send_json(ok ? 200 : 404, out);
}

// ---- /api/cmd ----------------------------------------------------------

static void handle_cmd() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    String line = server.arg("plain");
    line.trim();
    if (line.length() == 0) {
        server.send(400, "text/plain", "empty command");
        return;
    }
    // Touch/gesture injection is intentionally NOT reachable over IP -
    // these commands run from BLE NUS / USB serial only. A network
    // attacker on the LAN should not be able to drive the UI.
    if (input_test::is_injection_command(line)) {
        server.send(403, "text/plain",
                    "injection commands are BLE/serial only");
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
    JsonDocument doc;
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
    ::config::UiConfig ui = ::config::ui();
    ::config::AlarmConfig al = ::config::alarms();
    ::config::SignalKConfig sk = ::config::signalk();

    JsonDocument doc;
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
    JsonDocument doc;
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

// ---- /api/screenshot.bmp -----------------------------------------------
// LVGL snapshot of the active screen. Requires LV_USE_SNAPSHOT in lv_conf.h.

static void handle_screenshot() {
    uint8_t *bmp = nullptr;
    size_t len = 0;
    if (!screenshot::request(2500, &bmp, &len) || !bmp || len == 0) {
        server.send(504, "text/plain", "snapshot timeout");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(len);
    server.send(200, "image/bmp", "");
    // Chunked write so we don't blast a 460+ kB payload in one shot.
    WiFiClient client = server.client();
    const uint8_t *p = bmp;
    size_t left = len;
    while (left && client.connected()) {
        size_t chunk = left > 1460 ? 1460 : left;
        size_t w = client.write(p, chunk);
        if (w == 0) break;
        p += w;
        left -= w;
    }
    heap_caps_free(bmp);
}

// ---- root HTML page ----------------------------------------------------
// Self-contained vanilla HTML+JS. No external CDN. All DOM writes go
// through textContent or createElement/append - no innerHTML with values.

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>esp32-boat-mfd</title>
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
  input{background:#05101c;color:#eaf2ff;border:1px solid #223a55;border-radius:4px;padding:4px 8px;font-family:monospace}
  .status{color:#33d17a}
  .status.stalled{color:#ffb84d}
  .status.disconnected{color:#ff4d6d}
</style>
<h1>esp32-boat-mfd</h1>
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
    <div class=k>LAYOUT JSON</div>
    <textarea id=layout></textarea>
    <button id=layoutApply>apply</button>
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
    const r = await fetch('/api/layout');
    if (!r.ok) { document.getElementById('layoutMsg').textContent = 'no layout'; return; }
    const t = await r.text();
    document.getElementById('layout').value = JSON.stringify(JSON.parse(t), null, 2);
    document.getElementById('layoutMsg').textContent = '';
  }catch(e){ document.getElementById('layoutMsg').textContent = String(e.message||e); }
}
async function saveLayout(){
  const t = document.getElementById('layout').value;
  const r = await fetch('/api/layout', {method:'PUT', headers:{'Content-Type':'application/json'}, body:t});
  document.getElementById('layoutMsg').textContent = (r.ok?'ok ':'fail ') + r.status;
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
for (const b of document.querySelectorAll('button[data-cmd]')) {
  b.addEventListener('click', () => sendCmd(b.getAttribute('data-cmd')));
}

let wifiPoll = null;
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

let brightT = null;
document.getElementById('brightSlider').addEventListener('input', e => {
  if (brightT) clearTimeout(brightT);
  brightT = setTimeout(() => sendCmd('bright ' + e.target.value), 120);
});

refresh();
loadLayout();
wifiSavedRefresh();
setInterval(refresh, 5000);
</script>
)HTML";

static void handle_root() {
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
    server.on("/api/config", HTTP_GET, handle_config);
    server.on("/api/config/status", HTTP_GET, handle_config_status);
    server.on("/api/screens", HTTP_GET, handle_screens);
    server.on("/api/sk", HTTP_GET, handle_sk_data);
    server.on("/api/boat", HTTP_GET, handle_boat);
    server.on("/api/commands", HTTP_GET, handle_commands_json);
    server.on("/help/commands", HTTP_GET, handle_commands_html);
    server.on("/api/layout", HTTP_GET, handle_layout_get);
    server.on("/api/layout", HTTP_PUT, handle_layout_put);
    server.on("/api/cmd", HTTP_POST, handle_cmd);
    server.on("/api/wifi/scan", HTTP_POST, handle_wifi_scan);
    server.on("/api/wifi/networks", HTTP_GET, handle_wifi_networks);
    server.on("/api/wifi/connect", HTTP_POST, handle_wifi_connect);
    server.on("/api/wifi/forget", HTTP_POST, handle_wifi_forget);
    server.on("/api/wifi/saved", HTTP_GET, handle_wifi_saved_get);
    server.on("/api/screenshot.bmp", HTTP_GET, handle_screenshot);
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
            server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
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
        net::logf("[web] http %s on :80 for wifi=%s",
                  first ? "bound" : "rebound", net::wifiStateName());
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
    net::logf("[web] http task on core %d (deferring bind until wifi up)",
              xPortGetCoreID());
    for (;;) {
        sync_captive_dns();
        if (captive_active) dns.processNextRequest();
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void setup() {
    if (started) return;
    bind_routes();
    BaseType_t r = xTaskCreatePinnedToCore(web_task, "web", 8192, nullptr,
                                           1 /* low prio */, &s_task, 0 /* core 0 */);
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
