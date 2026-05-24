#include "signalk.h"
#include "signalk_parser.h"
#include "net.h"

#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

namespace sk {

Data data;

static WebSocketsClient ws;
static Preferences prefs;
static String s_host = "";
static uint16_t s_port = 3000;
static bool subscribed = false;

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
    int n = applyDelta((const char *)payload, len, data);
    if (n > 0) data.lastUpdateMs = millis();
}

static void onEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        net::logf("[sk] WS connected to %s:%u", s_host.c_str(), s_port);
        data.connected = true;
        subscribed = false;
        subscribe();
        break;
    case WStype_DISCONNECTED:
        net::logf("[sk] WS disconnected");
        data.connected = false;
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

void setup(const String &host, uint16_t port) {
    prefs.begin("sk", false);
    s_host = prefs.getString("host", host);
    s_port = (uint16_t)prefs.getUInt("port", port);
    if (s_host.length() == 0) {
        net::logf("[sk] no host configured - use 'sk <host> [port]'");
        return;
    }
    net::logf("[sk] target %s:%u", s_host.c_str(), s_port);
    ws.begin(s_host.c_str(), s_port, "/signalk/v1/stream?subscribe=none");
    ws.onEvent(onEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);
}

void loop() {
    ws.loop();
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
    if (line == "sk-status") {
        net::logf("host=%s port=%u connected=%d lastUpdateAgo=%lums", s_host.c_str(), s_port,
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

String connectionStatus() {
    if (!data.connected) return "disconnected";
    uint32_t ago = millis() - data.lastUpdateMs;
    if (data.lastUpdateMs == 0 || ago > 10000) return "stalled";
    return "live";
}

}  // namespace sk
