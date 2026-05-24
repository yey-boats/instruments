#include "ble_config.h"
#include "net.h"
#include "signalk.h"
#include "layout_loader.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

namespace bleconfig {

static NimBLECharacteristic *s_conn = nullptr;
static NimBLECharacteristic *s_config = nullptr;

// Build the JSON document returned on CONNECTION reads.
static String connectionJson() {
    JsonDocument doc;
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["mode"] = net::wifiUp() ? "STA" : "AP";
    wifi["ssid"] = WiFi.SSID();
    wifi["ip"] = net::ipString();
    wifi["rssi"] = net::rssi();

    JsonObject sk = doc["sk"].to<JsonObject>();
    Preferences prefs;
    prefs.begin("sk", true);
    sk["host"] = prefs.getString("host", "");
    sk["port"] = prefs.getUInt("port", 3000);
    prefs.end();
    sk["state"] = sk::connectionStatus();

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["uptime_ms"] = millis();
    dev["heap_free"] = (uint32_t)ESP.getFreeHeap();
    dev["psram_free"] = (uint32_t)ESP.getFreePsram();

    String out;
    serializeJson(doc, out);
    return out;
}

// Apply a CONNECTION write. Phone app sends a partial JSON like:
//   {"wifi":{"ssid":"X","password":"Y"}, "sk":{"host":"...","port":3000}}
// Empty / missing fields are left alone.
static void applyConnectionWrite(const std::string &data) {
    JsonDocument doc;
    if (deserializeJson(doc, data)) {
        net::logf("[bleconfig] connection write: invalid JSON");
        return;
    }
    JsonVariantConst wifi = doc["wifi"];
    if (!wifi.isNull()) {
        const char *ssid = wifi["ssid"];
        const char *pass = wifi["password"];
        if (ssid && pass) {
            String cmd = String("wifi ") + ssid + " " + pass;
            net::logf("[bleconfig] applying wifi config (will reboot)");
            net::handleSerialCommand(cmd);  // saves + reboots
            return;                         // we won't reach here
        }
        if (wifi["forget"] | false) {
            net::handleSerialCommand("wifi-forget");
            return;
        }
    }
    JsonVariantConst skc = doc["sk"];
    if (!skc.isNull()) {
        const char *host = skc["host"];
        if (host) {
            uint16_t port = skc["port"] | 3000;
            String cmd = String("sk ") + host + " " + String(port);
            net::logf("[bleconfig] applying sk target (will reboot)");
            sk::handleSerialCommand(cmd);
            return;
        }
    }
    net::logf("[bleconfig] connection write: no actionable fields");
}

class ConnCb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *c) override {
        String j = connectionJson();
        c->setValue((const uint8_t *)j.c_str(), j.length());
    }
    void onWrite(NimBLECharacteristic *c) override { applyConnectionWrite(c->getValue()); }
};

class ConfigCb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *c) override {
        // BLE attribute values are capped at 512 bytes (NimBLE / BLE spec).
        // Layouts that exceed this can't be transferred in a single GATT read
        // here - phone apps should use the SignalK REST endpoint or chunked
        // writes for large layouts. We return either the full layout (if it
        // fits) or a compact summary indicating size + screen count.
        size_t len = 0;
        const char *j = layout::last_json(&len);
        if (j && len && len <= 512) {
            c->setValue((const uint8_t *)j, len);
            return;
        }
        // Fall back to summary: total size + screen list
        JsonDocument doc;
        doc["truncated"] = true;
        doc["size"] = (uint32_t)len;
        if (layout::loaded()) {
            const layout::Config &cfg = layout::current();
            doc["screen_count"] = cfg.screen_count;
            doc["alarm_count"] = cfg.alarm_count;
            doc["default_screen"] = cfg.settings.default_screen;
        }
        String out;
        serializeJson(doc, out);
        c->setValue((const uint8_t *)out.c_str(), out.length());
    }
    void onWrite(NimBLECharacteristic *c) override {
        std::string data = c->getValue();
        net::logf("[bleconfig] configuration write: %u bytes", (unsigned)data.size());
        if (data.empty()) return;
        if (data.size() > 512) {
            net::logf("[bleconfig] configuration write: rejected (size > 512)");
            return;
        }
        layout::apply_json(data.data(), data.size());
    }
};

void setup() {
    NimBLEServer *server = NimBLEDevice::createServer();  // returns existing if already created
    NimBLEService *svc = server->createService(SERVICE_UUID);

    s_conn = svc->createCharacteristic(CONN_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                                                      NIMBLE_PROPERTY::NOTIFY);
    s_conn->setCallbacks(new ConnCb());
    {
        String j = connectionJson();
        s_conn->setValue((const uint8_t *)j.c_str(), j.length());
    }

    s_config = svc->createCharacteristic(
        CONFIG_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    s_config->setCallbacks(new ConfigCb());
    {
        size_t len = 0;
        const char *j = layout::last_json(&len);
        if (j && len) s_config->setValue((uint8_t *)j, len);
    }

    svc->start();

    // Advertise the new service alongside the existing Nordic UART.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);

    net::logf("[bleconfig] GATT service registered (%s)", SERVICE_UUID);
}

void notifyAll() {
    if (s_conn) {
        String j = connectionJson();
        s_conn->setValue((const uint8_t *)j.c_str(), j.length());
        s_conn->notify();
    }
    if (s_config) {
        size_t len = 0;
        const char *j = layout::last_json(&len);
        if (j && len && len <= 512) {
            s_config->setValue((const uint8_t *)j, len);
            s_config->notify();
        }
        // For layouts > 512 B, the read callback returns a summary; no notify
        // to avoid spamming the NimBLE "value exceeds max" error.
    }
}

}  // namespace bleconfig
