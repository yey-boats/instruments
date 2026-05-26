#include "net.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <freertos/semphr.h>

#include "secrets.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ble_config.h"
#include "wifi_store.h"
#include "app_events.h"

namespace net {

static Preferences prefs;
static WiFiUDP udp;
static IPAddress broadcastAddr;
static bool ota_started = false;
static bool ap_mode = false;
static ExtraCommandHandler s_extra = nullptr;
static String s_device_id;
static volatile WifiState s_wifi_state = WifiState::Idle;
static TaskHandle_t s_wifi_task = nullptr;
static TaskHandle_t s_ble_adv_task = nullptr;
static SemaphoreHandle_t s_log_mtx = nullptr;

WifiState wifiState() { return s_wifi_state; }

const char *wifiStateName() {
    switch (s_wifi_state) {
    case WifiState::Idle: return "idle";
    case WifiState::Connecting: return "connecting";
    case WifiState::StaUp: return "sta";
    case WifiState::ApSetup: return "ap";
    case WifiState::Failed: return "failed";
    }
    return "?";
}

const String &deviceId() {
    return s_device_id;
}

// ---- BLE ----
// Nordic UART service UUIDs (de facto BLE serial standard).
// clang-format off
#define NUS_SERVICE     "6e400001-b5a3-f393-e0a3-9f4dd9e3a05a"
#define NUS_TX_CHAR_RX  "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"  // peer writes here (host->device)
#define NUS_TX_CHAR_TX  "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"  // device notifies here (device->host)
// clang-format on

static NimBLECharacteristic *bleTxChar = nullptr;
static NimBLECharacteristic *bleRxChar = nullptr;
static volatile bool bleConnected = false;

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *) override {
        bleConnected = true;
        logf("[ble] connected");
    }
    void onDisconnect(NimBLEServer *s) override {
        bleConnected = false;
        logf("[ble] disconnected");
    }
};

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        String line(v.c_str());
        line.trim();
        if (line.length() == 0) return;
        logf("[ble] rx: %s", line.c_str());
        // BLE callbacks run on the NimBLE task and must stay short. Most
        // commands touch LVGL or NVS; queue them for the UI task instead
        // of executing in-place. Read-only / status commands (sk-status,
        // sk-dump, ip, bench, screen, wifi-list, bright (read)) are fast
        // enough to keep inline so the immediate BLE-stream response
        // doesn't lag.
        // `scan` removed from the inline allow-list: WiFi.scanNetworks
        // blocks the NimBLE callback task. It now runs on the UI/net
        // queue path (with async scan support already wired in the web
        // UI), but a console-issued `scan` still routes through there
        // cleanly via dispatchCommand.
        bool inline_ok = (line == "ip" || line == "bench" || line == "screen" ||
                          line == "sk-status" || line == "sk-dump" ||
                          line == "wifi-list" || line == "bright");
        if (inline_ok) {
            if (!handleSerialCommand(line) && !sk::handleSerialCommand(line) &&
                !layout::handleSerialCommand(line)) {
                if (s_extra) s_extra(line);
            }
            return;
        }
        app::Command cmd;
        cmd.type = app::CommandType::RunCommand;
        strncpy(cmd.a, line.c_str(), sizeof(cmd.a) - 1);
        if (!app::post(cmd, 50)) {
            logf("[ble] queue full, dropping command");
        }
    }
};

static void ble_advertising_watchdog(void *) {
    for (;;) {
        if (!bleConnected) {
            NimBLEDevice::startAdvertising();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void bleSetup() {
    NimBLEDevice::init(s_device_id.c_str());
    NimBLEDevice::setMTU(247);
    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    NimBLEService *svc = server->createService(NUS_SERVICE);
    bleTxChar = svc->createCharacteristic(NUS_TX_CHAR_TX, NIMBLE_PROPERTY::NOTIFY);
    bleRxChar = svc->createCharacteristic(NUS_TX_CHAR_RX,
                                          NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleRxChar->setCallbacks(new RxCb());
    svc->start();

    bleconfig::setup();  // adds the Connection + Configuration GATT service

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.printf("[ble] advertising as %s\n", s_device_id.c_str());
    xTaskCreatePinnedToCore(ble_advertising_watchdog, "ble-adv", 3072, nullptr, 1,
                            &s_ble_adv_task, 0);
}

// ---- WiFi / OTA ----
static void otaSetup() {
    ArduinoOTA.setHostname(s_device_id.c_str());
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        Serial.printf("[ota] start (%s)\n", ArduinoOTA.getCommand() == U_FLASH ? "flash" : "fs");
    });
    ArduinoOTA.onEnd([]() { Serial.println("[ota] end"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        static unsigned int last = 0;
        unsigned int pct = (p * 100) / t;
        if (pct != last) {
            Serial.printf("[ota] %u%%\n", pct);
            last = pct;
        }
    });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %d\n", e); });
    ArduinoOTA.begin();
    ota_started = true;
    Serial.printf("[ota] ready at %s.local\n", s_device_id.c_str());
}

// Try one network with a 10s timeout. Returns true on association.
static bool try_join(const char *ssid, const char *pass) {
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_device_id.c_str());
    Serial.printf("[wifi] trying '%s' (pass len %u)\n", ssid, (unsigned)strlen(pass ? pass : ""));
    if (pass && *pass)
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

static void start_ap_mode() {
    Serial.println("[wifi] starting AP mode (full reset)");
    // Fully tear down any lingering STA state. Without this, after one or
    // more failed try_join calls the lwIP / DHCP server may not initialise
    // on the AP interface and clients get "could not obtain IP".
    WiFi.persistent(false);
    WiFi.disconnect(true /* wifioff */, true /* erase */);
    delay(120);
    WiFi.mode(WIFI_OFF);
    delay(180);
    WiFi.mode(WIFI_AP);
    delay(120);

    IPAddress ip(192, 168, 4, 1);
    IPAddress gw(192, 168, 4, 1);
    IPAddress mask(255, 255, 255, 0);
    if (!WiFi.softAPConfig(ip, gw, mask)) {
        Serial.println("[wifi] softAPConfig FAILED");
    }
    // channel 1, not hidden, max 4 clients, beacon interval default.
    bool ok = WiFi.softAP("espdisp-setup", nullptr, 1, 0, 4);
    delay(500);  // allow the DHCP server task to spin up
    if (!ok) {
        Serial.println("[wifi] softAP FAILED");
    }
    broadcastAddr = WiFi.softAPIP();
    broadcastAddr[3] = 255;
    ap_mode = true;
    Serial.printf("[wifi] AP ip=%s  mac=%s  ssid='espdisp-setup' (open)\n",
                  WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
    Serial.printf("[wifi] connected stations: %d\n", WiFi.softAPgetStationNum());
}

// Tell the UI to open the WiFi setup screen (used when we fall into AP
// mode). Posted via the app event queue so the UI task does the LVGL
// work. Safe to call from this manager task.
static void post_show_wifi_screen() {
    app::Command cmd;
    cmd.type = app::CommandType::ShowScreen;
    strncpy(cmd.a, "wifi", sizeof(cmd.a) - 1);
    app::post(cmd, 200);
}

static void wifi_manager_task(void *) {
    s_wifi_state = WifiState::Connecting;
    wifi_store::load();
    wifi_store::migrate_legacy_if_any();

    if (wifi_store::count() == 0) {
        Serial.println("[wifi] no saved networks, AP-only");
        start_ap_mode();
        udp.begin(UDP_LOG_PORT);
        s_wifi_state = WifiState::ApSetup;
        post_show_wifi_screen();
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("[wifi] %u saved network%s, trying in order\n",
                  (unsigned)wifi_store::count(), wifi_store::count() == 1 ? "" : "s");

    bool joined = false;
    for (size_t i = 0; i < wifi_store::count(); ++i) {
        const auto &n = wifi_store::at(i);
        if (try_join(n.ssid, n.pass)) {
            joined = true;
            break;
        }
    }

    if (joined) {
        IPAddress ip = WiFi.localIP();
        broadcastAddr = ip;
        broadcastAddr[3] = 255;
        Serial.printf("[wifi] up: ip=%s  ssid='%s'  rssi=%d\n", ip.toString().c_str(),
                      WiFi.SSID().c_str(), WiFi.RSSI());
        if (MDNS.begin(s_device_id.c_str())) {
            MDNS.addService("arduino", "tcp", 3232);
            Serial.printf("[mdns] %s.local\n", s_device_id.c_str());
        }
        otaSetup();
        udp.begin(UDP_LOG_PORT);
        s_wifi_state = WifiState::StaUp;
    } else {
        Serial.println("[wifi] all saved networks failed, fallback to AP");
        start_ap_mode();
        udp.begin(UDP_LOG_PORT);
        s_wifi_state = WifiState::ApSetup;
        post_show_wifi_screen();
    }
    vTaskDelete(NULL);
}

void setup() {
    if (!s_log_mtx) s_log_mtx = xSemaphoreCreateMutex();
    prefs.begin("net", false);
    s_device_id = prefs.getString("device_id", OTA_HOSTNAME);
    Serial.printf("[net] device id: %s\n", s_device_id.c_str());
    // BLE is independent of WiFi - bring it up immediately so the BLE
    // console is responsive even if the WiFi manager is still trying.
    bleSetup();
    // WiFi join attempts run on their own task (Phase 4). With multiple
    // saved networks and 10 s per try this could be 30+ seconds blocking
    // the main loop; making it async lets the UI render immediately.
    xTaskCreatePinnedToCore(wifi_manager_task, "wifi-mgr", 6144, nullptr, 1,
                            &s_wifi_task, 0);
}

bool dispatchCommand(const String &line) {
    if (handleSerialCommand(line)) return true;
    if (sk::handleSerialCommand(line)) return true;
    if (layout::handleSerialCommand(line)) return true;
    if (s_extra && s_extra(line)) return true;
    return false;
}

void loop() {
    if (ota_started) ArduinoOTA.handle();
}

bool wifiUp() {
    return WiFi.status() == WL_CONNECTED;
}
String ipString() {
    return ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

int rssi() {
    return wifiUp() ? WiFi.RSSI() : 0;
}

void setExtraCommandHandler(ExtraCommandHandler h) {
    s_extra = h;
}

void saveWifi(const String &ssid, const String &pass) {
    wifi_store::put(ssid.c_str(), pass.c_str());
    logf("[wifi] saved ssid='%s' (pass len %u, total %u nets) - rebooting", ssid.c_str(),
         (unsigned)pass.length(), (unsigned)wifi_store::count());
    delay(200);
    ESP.restart();
}

void logf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (n == 0) return;

    bool locked = false;
    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        locked = true;
    }
    Serial.write((uint8_t *)buf, n);
    if (buf[n - 1] != '\n') Serial.println();

    if (wifiUp() || ap_mode) {
        udp.beginPacket(broadcastAddr, UDP_LOG_PORT);
        udp.write((uint8_t *)buf, n);
        udp.endPacket();
    }
    if (bleConnected && bleTxChar) {
        bleTxChar->setValue((uint8_t *)buf, n);
        bleTxChar->notify();
    }
    if (locked) xSemaphoreGive(s_log_mtx);
}

bool handleSerialCommand(const String &line) {
    if (line.startsWith("wifi ")) {
        // Accept either:
        //   wifi <ssid>                          (open, no spaces in ssid)
        //   wifi <ssid> <pass>                   (no spaces in ssid)
        //   wifi "<ssid with spaces>" [pass]     (any ssid)
        String ssid, pass;
        const char *p = line.c_str() + 5;
        while (*p == ' ') p++;
        if (*p == '"') {
            const char *start = p + 1;
            const char *end = strchr(start, '"');
            if (!end) {
                logf("usage: wifi \"<ssid>\" [pass]   (missing closing quote)");
                return true;
            }
            ssid = String(start).substring(0, end - start);
            p = end + 1;
            while (*p == ' ') p++;
            pass = String(p);
        } else {
            const char *sp = strchr(p, ' ');
            if (!sp) {
                ssid = String(p);
                pass = "";
            } else {
                ssid = String(p).substring(0, sp - p);
                pass = String(sp + 1);
            }
        }
        ssid.trim();
        if (ssid.length() == 0) {
            logf("usage: wifi <ssid> [pass]  or  wifi \"<ssid>\" [pass]");
            return true;
        }
        saveWifi(ssid, pass);
        return true;
    }
    if (line == "wifi-forget") {
        wifi_store::clear_all();
        prefs.remove("ssid");
        prefs.remove("pass");
        logf("[wifi] all networks cleared - rebooting");
        delay(200);
        ESP.restart();
        return true;
    }
    if (line.startsWith("wifi-forget ")) {
        String ssid = line.substring(12);
        ssid.trim();
        bool ok = wifi_store::remove(ssid.c_str());
        logf("[wifi] forget '%s' -> %s", ssid.c_str(), ok ? "removed" : "not found");
        return true;
    }
    if (line == "wifi-list") {
        logf("[wifi] %u saved network%s:", (unsigned)wifi_store::count(),
             wifi_store::count() == 1 ? "" : "s");
        for (size_t i = 0; i < wifi_store::count(); ++i) {
            const auto &n = wifi_store::at(i);
            logf("  [%u] %s  (pass: %s)", (unsigned)i, n.ssid, n.pass[0] ? "yes" : "no");
        }
        return true;
    }
    if (line == "ip") {
        logf("ip=%s  mode=%s  rssi=%d", ipString().c_str(), ap_mode ? "AP" : "STA", WiFi.RSSI());
        return true;
    }
    if (line == "reboot") {
        ESP.restart();
        return true;
    }
    if (line == "id") {
        logf("[net] device id: %s", s_device_id.c_str());
        return true;
    }
    if (line.startsWith("id ")) {
        String id = line.substring(3);
        id.trim();
        if (id.length() == 0 || id.length() > 31) {
            logf("[net] id: name must be 1..31 chars");
            return true;
        }
        prefs.putString("device_id", id);
        logf("[net] device id -> '%s' (rebooting)", id.c_str());
        delay(200);
        ESP.restart();
        return true;
    }
    if (line == "scan") {
        logf("[scan] starting...");
        int n = WiFi.scanNetworks(false, true);
        logf("[scan] found %d networks (2.4GHz only):", n);
        for (int i = 0; i < n; ++i) {
            logf("  %2d  %s  rssi=%d  ch=%d  %s", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                 WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
        }
        WiFi.scanDelete();
        return true;
    }
    return false;
}

}  // namespace net
