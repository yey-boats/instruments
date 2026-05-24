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

namespace net {

static Preferences prefs;
static WiFiUDP udp;
static IPAddress broadcastAddr;
static bool ota_started = false;
static bool ap_mode = false;

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
                if (!handleSerialCommand(line)) {
                    sk::handleSerialCommand(line);
                }
            }
        }
    }
};

static void bleSetup() {
    NimBLEDevice::init(OTA_HOSTNAME);
    NimBLEDevice::setMTU(247);
    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    NimBLEService *svc = server->createService(NUS_SERVICE);
    bleTxChar = svc->createCharacteristic(NUS_TX_CHAR_TX, NIMBLE_PROPERTY::NOTIFY);
    bleRxChar = svc->createCharacteristic(NUS_TX_CHAR_RX,
                                          NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleRxChar->setCallbacks(new RxCb());
    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.println("[ble] advertising as " OTA_HOSTNAME);
}

// ---- WiFi / OTA ----
static void otaSetup() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
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
    Serial.printf("[ota] ready at %s.local\n", OTA_HOSTNAME);
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
    WiFi.setHostname(OTA_HOSTNAME);
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
        if (MDNS.begin(OTA_HOSTNAME)) {
            MDNS.addService("arduino", "tcp", 3232);
            Serial.printf("[mdns] %s.local\n", OTA_HOSTNAME);
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
    wifiStart();
    bleSetup();
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
        int sp = line.indexOf(' ', 5);
        if (sp < 0) {
            logf("usage: wifi <ssid> <pass>");
            return true;
        }
        String ssid = line.substring(5, sp);
        String pass = line.substring(sp + 1);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        logf("[wifi] saved ssid='%s' (len %d) - rebooting", ssid.c_str(), pass.length());
        delay(200);
        ESP.restart();
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
