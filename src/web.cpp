#include "web.h"

#include <Arduino.h>
#include <WebServer.h>
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

namespace web {

static WebServer server(80);
static bool started = false;

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
    wifi["ssid"] = WiFi.SSID();
    wifi["ip"] = net::ipString();
    wifi["rssi"] = net::rssi();

    JsonObject sk = doc["sk"].to<JsonObject>();
    sk["state"] = sk::connectionStatus();
    Preferences p;
    p.begin("sk", true);
    sk["host"] = p.getString("host", "");
    sk["port"] = p.getUInt("port", 3000);
    p.end();

    JsonObject screen = doc["screen"].to<JsonObject>();
    screen["index"] = ui::current_index();
    screen["id"] = ui::current_id();
    screen["title"] = ui::current_title();
    screen["count"] = (uint32_t)ui::screen_count();

    send_json(200, doc);
}

// ---- /api/screens, /api/screen/<id> ------------------------------------

static void handle_screens() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    int saved = ui::current_index();
    for (size_t i = 0; i < ui::screen_count(); ++i) {
        ui::show((int)i);
        JsonObject s = arr.add<JsonObject>();
        s["index"] = (uint32_t)i;
        s["id"] = ui::current_id();
        s["title"] = ui::current_title();
        s["hidden"] = ui::is_hidden(i);
        s["active"] = (saved == (int)i);
    }
    ui::show(saved);
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
    if (id == "next") ui::next();
    else if (id == "prev") ui::prev();
    else if (id.length() && isdigit(id[0])) ui::show(id.toInt());
    else if (!ui::show_by_id(id.c_str())) {
        server.send(404, "text/plain", "unknown screen id");
        return;
    }
    JsonDocument doc;
    doc["index"] = ui::current_index();
    doc["id"] = ui::current_id();
    send_json(200, doc);
}

// ---- /api/sk -----------------------------------------------------------

static void put_double(JsonObject o, const char *k, double v) {
    if (!isnan(v)) o[k] = v;
}

static void handle_sk_data() {
    const sk::Data &d = sk::data;
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

// ---- /api/layout (GET / PUT) -------------------------------------------

static void handle_layout_get() {
    size_t len = 0;
    const char *j = layout::last_json(&len);
    if (!j || !len) {
        server.send(404, "text/plain", "no layout loaded");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", String(j));
}

static void handle_layout_put() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    const String &body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    if (!layout::apply_json(body.c_str(), body.length())) {
        server.send(400, "text/plain", "layout parse / apply failed");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["size"] = (uint32_t)body.length();
    send_json(200, doc);
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
    bool handled = net::dispatchCommand(line);
    JsonDocument doc;
    doc["handled"] = handled;
    doc["cmd"] = line;
    send_json(handled ? 200 : 422, doc);
}

// ---- /api/screenshot.bmp -----------------------------------------------
// LVGL snapshot of the active screen. Requires LV_USE_SNAPSHOT in lv_conf.h.

static void handle_screenshot() {
#if LV_USE_SNAPSHOT
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_NATIVE);
    if (!buf) {
        server.send(500, "text/plain", "snapshot failed");
        return;
    }
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint32_t pix_bytes = stride * h;
    uint32_t total = 14 + 40 + 12 + pix_bytes;
    uint8_t hdr[14 + 40 + 12] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = total & 0xff; hdr[3] = (total >> 8) & 0xff;
    hdr[4] = (total >> 16) & 0xff; hdr[5] = (total >> 24) & 0xff;
    hdr[10] = (14 + 40 + 12) & 0xff;
    hdr[14] = 40;
    hdr[18] = w & 0xff; hdr[19] = (w >> 8) & 0xff;
    int32_t neg_h = -(int32_t)h;
    hdr[22] = neg_h & 0xff; hdr[23] = (neg_h >> 8) & 0xff;
    hdr[24] = (neg_h >> 16) & 0xff; hdr[25] = (neg_h >> 24) & 0xff;
    hdr[26] = 1;
    hdr[28] = 16;
    hdr[30] = 3;
    hdr[34] = pix_bytes & 0xff;
    hdr[35] = (pix_bytes >> 8) & 0xff;
    hdr[36] = (pix_bytes >> 16) & 0xff;
    hdr[37] = (pix_bytes >> 24) & 0xff;
    hdr[54] = 0x00; hdr[55] = 0xF8;
    hdr[58] = 0xE0; hdr[59] = 0x07;
    hdr[62] = 0x1F;

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.setContentLength(total);
    server.send(200, "image/bmp", "");
    server.client().write(hdr, sizeof(hdr));
    server.client().write((const uint8_t *)buf->data, pix_bytes);
    lv_draw_buf_destroy(buf);
#else
    server.send(501, "text/plain", "LV_USE_SNAPSHOT not enabled");
#endif
}

// ---- root HTML page ----------------------------------------------------
// Self-contained vanilla HTML+JS. No external CDN. All DOM writes go
// through textContent or createElement/append - no innerHTML with values.

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
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
  </div>
  <div class=card>
    <div class=k>COMMAND</div>
    <input id=cmd placeholder="e.g. sk-status"/>
    <button id=cmdSend>send</button>
    <pre id=cmdOut></pre>
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
refresh();
loadLayout();
setInterval(refresh, 2000);
</script>
)HTML";

static void handle_root() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", FPSTR(INDEX_HTML));
}

// ---- setup / loop ------------------------------------------------------

static void bind_routes() {
    server.on("/", HTTP_GET, handle_root);
    server.on("/api/state", HTTP_GET, handle_state);
    server.on("/api/screens", HTTP_GET, handle_screens);
    server.on("/api/sk", HTTP_GET, handle_sk_data);
    server.on("/api/layout", HTTP_GET, handle_layout_get);
    server.on("/api/layout", HTTP_PUT, handle_layout_put);
    server.on("/api/cmd", HTTP_POST, handle_cmd);
    server.on("/api/screenshot.bmp", HTTP_GET, handle_screenshot);
    server.onNotFound([]() {
        if (server.method() == HTTP_POST && server.uri().startsWith("/api/screen/")) {
            handle_screen_set();
            return;
        }
        if (server.method() == HTTP_OPTIONS) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS");
            server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
            server.send(204);
            return;
        }
        server.send(404, "text/plain", "not found");
    });
}

void setup() {
    if (started) return;
    bind_routes();
    server.begin();
    started = true;
    net::logf("[web] http server up on :80");
}

void loop() {
    if (started) server.handleClient();
}

}  // namespace web
