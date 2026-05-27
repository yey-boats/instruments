#include "signalk.h"
#include "signalk_parser.h"
#include "source_signalk.h"
#include "net.h"

#include <Preferences.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace sk {

Data data;
// Mutex guards mutation of `data` from the WS event task vs reads from
// UI/web. Reads should use sk::copyData(out) which takes a short
// critical section. Direct `sk::data` reads are still tolerated -
// they're 64-bit doubles that the ESP32 can corrupt under contention,
// but a torn read is just one stale frame.
static SemaphoreHandle_t s_data_mtx = nullptr;

static WebSocketsClient ws;
static Preferences prefs;
static String s_host = "";
static uint16_t s_port = 3000;
// Optional JWT for SignalK servers in token-security mode. Loaded
// once at setup() from NVS; updated in-place by the `sk-token` CLI
// (which then reboots so the WS reconnects with the new token).
// Stored in plain text in NVS - acceptable for a single-user marine
// device; never echoed back via /api/state or sk-status (only the
// length is exposed).
static String s_token = "";
// "auto" mode = no manual host saved -> we'll poll mDNS for
// _signalk-ws._tcp.local. and use the first record found. The user can
// pin a manual host with `sk-host manual <host>` (or the legacy `sk
// <host>`) which clears auto mode by saving a host.
static bool s_auto_mode = true;
static uint32_t s_last_discover_ms = 0;
static bool subscribed = false;
static TaskHandle_t s_sk_task = nullptr;
static volatile bool s_running = false;
static bool s_ws_started = false;
// Diagnostic counters - how often the SK task drains ws.loop() and how
// long it spends inside. Exposed via /api/state.sk for profiling.
static volatile uint32_t s_loop_iters = 0;
static volatile uint32_t s_loop_max_us = 0;

static void subscribe() {
    if (subscribed) return;
    JsonDocument sub;
    sub["context"] = "vessels.self";
    JsonArray arr = sub["subscribe"].to<JsonArray>();
    const char *paths[] = {
        "navigation.position",
        "navigation.speedOverGround",
        "navigation.speedThroughWater",
        "navigation.courseOverGroundTrue",
        "navigation.headingTrue",
        "environment.wind.angleApparent",
        "environment.wind.speedApparent",
        "environment.wind.angleTrueWater",
        "environment.wind.speedTrue",
        "environment.depth.belowTransducer",
        "environment.depth.belowKeel",
        "environment.water.temperature",
        "electrical.batteries.house.voltage",
        "electrical.batteries.house.stateOfCharge",
        "tanks.fuel.0.currentLevel",
        "tanks.freshWater.0.currentLevel",
        "navigation.courseRhumbline.crossTrackError",
        "navigation.courseRhumbline.bearingTrackTrue",
        "navigation.courseRhumbline.nextPoint.bearingTrue",
        "navigation.courseRhumbline.nextPoint.distance",
        "navigation.courseRhumbline.velocityMadeGood",
        "steering.autopilot.target.headingTrue",
        "steering.autopilot.state",
        "environment.current.setTrue",
        "environment.current.drift",
    };
    for (auto p : paths) {
        JsonObject o = arr.add<JsonObject>();
        o["path"] = p;
        o["period"] = 1000;
        o["minPeriod"] = 200;
        o["format"] = "delta";
        o["policy"] = "instant";
    }
    String out;
    serializeJson(sub, out);
    ws.sendTXT(out);
    subscribed = true;
    net::logf("[sk] subscribed to %d paths", (int)(sizeof(paths) / sizeof(paths[0])));
}

static void onText(uint8_t *payload, size_t len) {
    if (s_data_mtx) xSemaphoreTake(s_data_mtx, portMAX_DELAY);
    int n = applyDelta((const char *)payload, len, data);
    uint32_t now = millis();
    if (n > 0) data.lastUpdateMs = now;
    Data snap = data;
    if (s_data_mtx) xSemaphoreGive(s_data_mtx);
    // Bridge into the source-neutral model. boat::publish() does its own
    // locking; we hand it a snapshot so the SK mutex isn't held across
    // the priority-resolution path.
    if (n > 0) boat::bridge_signalk_into_boat(snap, now);
}

static void set_connected(bool v) {
    if (s_data_mtx) xSemaphoreTake(s_data_mtx, portMAX_DELAY);
    data.connected = v;
    if (s_data_mtx) xSemaphoreGive(s_data_mtx);
}

static void onEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        net::logf("[sk] WS connected to %s:%u", s_host.c_str(), s_port);
        set_connected(true);
        subscribed = false;
        subscribe();
        break;
    case WStype_DISCONNECTED:
        net::logf("[sk] WS disconnected");
        set_connected(false);
        subscribed = false;
        break;
    case WStype_TEXT:
        onText(payload, len);
        break;
    case WStype_ERROR:
        net::logf("[sk] WS error");
        break;
    default:
        break;
    }
}

void copyData(Data &out) {
    // Compose the visible snapshot from boat::Snapshot so callers see
    // the fused (priority-resolved, freshness-aware) value chosen across
    // SignalK / NMEA-WiFi / NMEA2000. lastUpdateMs and connected stay
    // sourced from the WS state.
    boat::compose_from_boat(out, millis());
    if (s_data_mtx) xSemaphoreTake(s_data_mtx, portMAX_DELAY);
    out.lastUpdateMs = data.lastUpdateMs;
    out.connected = data.connected;
    if (s_data_mtx) xSemaphoreGive(s_data_mtx);
}

// Start (or restart) the WebSocket client against the current
// s_host/s_port. Safe to call repeatedly - WebSocketsClient handles
// re-begin by closing the prior connection.
static void start_ws() {
    if (s_host.length() == 0) return;
    // Append ?token=<jwt> for SK servers in token-security mode.
    // Without a token the server may accept the connection but refuse
    // the subscribe; with one it works transparently. SK is happy with
    // either ? or & as the second separator (we already have subscribe).
    String path = "/signalk/v1/stream?subscribe=none";
    if (s_token.length()) path += "&token=" + s_token;
    ws.begin(s_host.c_str(), s_port, path.c_str());
    ws.onEvent(onEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);
    s_ws_started = true;
    net::logf("[sk] ws begin %s:%u token_len=%u",
              s_host.c_str(), s_port, (unsigned)s_token.length());
}

bool isAutoMode() { return s_auto_mode; }

bool tryAutoDiscover(uint32_t now_ms) {
    if (!s_auto_mode) return false;
    if (s_ws_started) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    // Throttle to one attempt per 15s; mDNS query is blocking ~1s.
    if (s_last_discover_ms && (now_ms - s_last_discover_ms) < 15000) return false;
    s_last_discover_ms = now_ms;
    int n = MDNS.queryService("signalk-ws", "tcp");
    if (n <= 0) {
        net::logf("[sk] mDNS: no _signalk-ws._tcp record found");
        return false;
    }
    String host = MDNS.hostname(0);
    IPAddress ip = MDNS.IP(0);
    uint16_t port = MDNS.port(0);
    if (host.length() == 0 && ip == IPAddress(0, 0, 0, 0)) return false;
    String target = ip != IPAddress(0, 0, 0, 0) ? ip.toString() : host;
    s_host = target;
    s_port = port ? port : 3000;
    net::logf("[sk] mDNS discovered signalk-ws at %s:%u (%d records)",
              s_host.c_str(), s_port, n);
    start_ws();
    return true;
}

// Dedicated SK task. Runs ws.loop() on core 0 so the synchronous
// WiFiClient::connect() (up to 5 s on a downed boat WiFi) cannot
// stall lv_timer_handler on core 1. The event callbacks (onEvent /
// onText / subscribe()) execute on this task; sk::data writes go
// through s_data_mtx and remain safe for UI/web readers.
static void sk_task(void *) {
    s_running = true;
    for (;;) {
        // Try auto-discovery whenever we have no target yet.
        if (!s_ws_started) tryAutoDiscover(millis());
        uint32_t t0 = micros();
        if (s_ws_started) ws.loop();
        uint32_t dt = micros() - t0;
        s_loop_iters++;
        if (dt > s_loop_max_us) s_loop_max_us = dt;
        // 10 ms cadence is plenty for WS frame draining and matches what
        // the old main-loop call site achieved via the loop's delay(5).
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup(const String &host, uint16_t port) {
    if (!s_data_mtx) s_data_mtx = xSemaphoreCreateMutex();
    prefs.begin("sk", false);
    s_host = prefs.getString("host", host);
    s_port = (uint16_t)prefs.getUInt("port", port);
    s_token = prefs.getString("token", "");
    // Auto mode iff no saved host. Manual mode persists across reboot
    // by virtue of a saved host being present.
    s_auto_mode = (s_host.length() == 0);
    if (s_auto_mode) {
        net::logf("[sk] auto-discovery enabled (no host saved); polling mDNS");
    } else {
        net::logf("[sk] manual target %s:%u", s_host.c_str(), s_port);
        start_ws();
    }

    if (!s_sk_task) {
        // Pin to core 0 (network/IO core). Stack 6 KB - WebSockets +
        // ArduinoJson subscribe doc need a few KB during connect.
        xTaskCreatePinnedToCore(sk_task, "sk", 6144, nullptr, 1,
                                &s_sk_task, 0);
    }
}

void loop() {
    // No-op. SK runs on its own task (sk_task) since blocking
    // WiFiClient::connect() during reconnect was stalling the main
    // Arduino loop and starving lv_timer_handler.
}

uint32_t loopIters() { return s_loop_iters; }
uint32_t loopMaxUs() {
    uint32_t v = s_loop_max_us;
    s_loop_max_us = 0;
    return v;
}

bool handleSerialCommand(const String &line) {
    if (line.startsWith("sk ")) {
        String rest = line.substring(3);
        rest.trim();
        int sp = rest.indexOf(' ');
        String host = sp < 0 ? rest : rest.substring(0, sp);
        uint16_t port = sp < 0 ? 3000 : (uint16_t)rest.substring(sp + 1).toInt();
        if (port == 0) port = 3000;
        prefs.putString("host", host);
        prefs.putUInt("port", port);
        net::logf("[sk] saved host=%s port=%u - rebooting", host.c_str(), port);
        delay(200);
        ESP.restart();
        return true;
    }
    if (line == "sk-host auto") {
        prefs.remove("host");
        prefs.remove("port");
        net::logf("[sk] auto-discovery enabled - rebooting");
        delay(200);
        ESP.restart();
        return true;
    }
    if (line.startsWith("sk-token")) {
        String rest = line.length() > 8 ? line.substring(8) : String("");
        rest.trim();
        if (rest.length() == 0) {
            // Status: never echo the token value, only its length.
            net::logf("[sk] token_len=%u", (unsigned)s_token.length());
            return true;
        }
        if (rest == "clear") {
            prefs.remove("token");
            net::logf("[sk] token cleared - rebooting");
            delay(200);
            ESP.restart();
            return true;
        }
        // Anything else is the JWT itself. Save + reboot so the WS
        // reconnects with the new credential.
        prefs.putString("token", rest);
        net::logf("[sk] token saved (len=%u) - rebooting",
                  (unsigned)rest.length());
        delay(200);
        ESP.restart();
        return true;
    }
    if (line == "sk-discover") {
        s_last_discover_ms = 0;  // unthrottle
        bool ok = tryAutoDiscover(millis());
        if (!ok) net::logf("[sk] discover: no record (or already started)");
        return true;
    }
    if (line == "sk-status") {
        net::logf("mode=%s host=%s port=%u token_len=%u connected=%d "
                  "lastUpdateAgo=%lums",
                  s_auto_mode ? "auto" : "manual",
                  s_host.c_str(), s_port,
                  (unsigned)s_token.length(),
                  data.connected, data.lastUpdateMs ? (millis() - data.lastUpdateMs) : 0);
        return true;
    }
    if (line == "sk-dump") {
        net::logf("lat=%.5f lon=%.5f", data.lat, data.lon);
        net::logf("sog=%.2f m/s (%.1f kn)  cog=%.3f rad  hdg=%.3f rad", data.sog,
                  isnan(data.sog) ? 0.0 : data.sog * 1.94384, data.cogTrue, data.headingTrue);
        net::logf("aws=%.2f m/s awa=%.3f rad  tws=%.2f twa=%.3f", data.aws, data.awa, data.tws,
                  data.twa);
        net::logf("depth=%.2fm  water=%.2fK  batt=%.2fV soc=%.2f", data.depth, data.waterTemp,
                  data.battVoltage, data.battSoc);
        return true;
    }
    return false;
}

int putValue(const char *path, const char *valueJson) {
    if (!path || !valueJson) return -1;
    if (s_host.length() == 0) {
        net::logf("[sk] PUT: no host configured");
        return -2;
    }
    if (WiFi.status() != WL_CONNECTED) {
        net::logf("[sk] PUT: wifi not up");
        return -3;
    }
    HTTPClient http;
    String url = String("http://") + s_host + ":" + String(s_port) +
                 "/signalk/v1/api/vessels/self/" + path;
    // SignalK PUT REST puts dots in path; we accept dot-paths in `path` arg
    // and send them as-is (the SK server expects them URL-encoded with
    // slashes between segments, but most servers accept dots too).
    if (!http.begin(url)) {
        net::logf("[sk] PUT: begin failed");
        return -4;
    }
    if (s_token.length()) http.addHeader("Authorization", String("Bearer ") + s_token);
    http.addHeader("Content-Type", "application/json");
    String body = String("{\"value\":") + valueJson + "}";
    int code = http.PUT(body);
    net::logf("[sk] PUT %s = %s -> %d", path, valueJson, code);
    http.end();
    return code;
}

String connectionStatus() {
    // Snapshot under the same mutex sk_task uses to mutate these fields.
    bool connected;
    uint32_t lastUpdate;
    if (s_data_mtx) xSemaphoreTake(s_data_mtx, portMAX_DELAY);
    connected = data.connected;
    lastUpdate = data.lastUpdateMs;
    if (s_data_mtx) xSemaphoreGive(s_data_mtx);
    if (!connected) return "disconnected";
    uint32_t ago = millis() - lastUpdate;
    if (lastUpdate == 0 || ago > 10000) return "stalled";
    return "live";
}

}  // namespace sk
