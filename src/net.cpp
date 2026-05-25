#include "net.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>

#include "secrets.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ble_config.h"

namespace net {

static Preferences prefs;
static WiFiUDP udp;
static IPAddress broadcastAddr;
static bool ota_started = false;
static bool ap_mode = false;
static ExtraCommandHandler s_extra = nullptr;
static String s_device_id;

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
static bool bleConnected = false;

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *) override { bleConnected = true; }
    void onDisconnect(NimBLEServer *s) override {
        bleConnected = false;
        NimBLEDevice::startAdvertising();
    }
};

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        if (!v.empty()) {
            String line(v.c_str());
            line.trim();
            if (line.length()) {
                Serial.printf("[ble] rx: %s\n", line.c_str());
                if (!handleSerialCommand(line) && !sk::handleSerialCommand(line) &&
                    !layout::handleSerialCommand(line)) {
                    if (s_extra) s_extra(line);
                }
            }
        }
    }
};

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

static void wifiStart() {
    String ssid = prefs.getString("ssid", WIFI_SSID);
    String pass = prefs.getString("pass", WIFI_PASS);

    if (ssid.length() == 0) {
        Serial.println("[wifi] no credentials, starting AP-only");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("espdisp-setup");
        broadcastAddr = WiFi.softAPIP();
        broadcastAddr[3] = 255;
        ap_mode = true;
        Serial.printf("[wifi] AP ip=%s  send 'wifi <ssid> <pass>' on serial to configure\n",
                      WiFi.softAPIP().toString().c_str());
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_device_id.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[wifi] connecting to '%s'", ssid.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        broadcastAddr = ip;
        broadcastAddr[3] = 255;
        Serial.printf("[wifi] up: ip=%s  rssi=%d\n", ip.toString().c_str(), WiFi.RSSI());
        if (MDNS.begin(s_device_id.c_str())) {
            MDNS.addService("arduino", "tcp", 3232);
            Serial.printf("[mdns] %s.local\n", s_device_id.c_str());
        }
        otaSetup();
    } else {
        Serial.println("[wifi] connect failed, fallback to AP");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("espdisp-setup");
        broadcastAddr = WiFi.softAPIP();
        broadcastAddr[3] = 255;
        ap_mode = true;
        Serial.printf("[wifi] AP ip=%s\n", WiFi.softAPIP().toString().c_str());
    }
    udp.begin(UDP_LOG_PORT);
}

void setup() {
    prefs.begin("net", false);
    s_device_id = prefs.getString("device_id", OTA_HOSTNAME);
    Serial.printf("[net] device id: %s\n", s_device_id.c_str());
    wifiStart();
    bleSetup();
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
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    logf("[wifi] saved ssid='%s' (pass len %u) - rebooting", ssid.c_str(),
         (unsigned)pass.length());
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
        prefs.remove("ssid");
        prefs.remove("pass");
        logf("[wifi] credentials cleared - rebooting");
        delay(200);
        ESP.restart();
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
