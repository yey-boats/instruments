#include "ble_config.h"
#include "net.h"
#include "psram_json.h"
#include "signalk.h"
#include "layout_loader.h"
#include "app_events.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include "storage.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

namespace bleconfig {

static NimBLECharacteristic *s_conn = nullptr;
static NimBLECharacteristic *s_config = nullptr;

// Build the JSON document returned on CONNECTION reads.
static String connectionJson() {
    JsonDocument doc(&espdisp::psram_json);
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["mode"] = net::wifiUp() ? "STA" : "AP";
    wifi["ssid"] = WiFi.SSID();
    wifi["ip"] = net::ipString();
    wifi["rssi"] = net::rssi();

    JsonObject sk = doc["sk"].to<JsonObject>();
    {
        storage::Namespace prefs("sk", true);
        sk["host"] = prefs.get_string("host", "");
        sk["port"] = prefs.get_u32("port", 3000);
    }
    sk["state"] = sk::connectionStatus();

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["id"] = net::deviceId();
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
    JsonDocument doc(&espdisp::psram_json);
    if (deserializeJson(doc, data)) {
        net::logf("[bleconfig] connection write: invalid JSON");
        return;
    }
    // Everything below this point is queued for the appropriate task -
    // BLE callbacks must stay short.
    JsonVariantConst wifi = doc["wifi"];
    if (!wifi.isNull()) {
        const char *ssid = wifi["ssid"];
        if (ssid && *ssid) {
            const char *pass = wifi["password"] | "";
            app::Command cmd;
            cmd.type = app::CommandType::SaveWifi;
            strncpy(cmd.a, ssid, sizeof(cmd.a) - 1);
            strncpy(cmd.b, pass, sizeof(cmd.b) - 1);
            app::post_net(cmd, 50);
            net::logf("[bleconfig] wifi save queued");
            return;
        }
        if (wifi["forget"] | false) {
            app::Command cmd;
            cmd.type = app::CommandType::RunCommand;
            strncpy(cmd.a, "wifi-forget", sizeof(cmd.a) - 1);
            app::post(cmd, 50);
            return;
        }
    }
    JsonVariantConst skc = doc["sk"];
    if (!skc.isNull()) {
        const char *host = skc["host"];
        if (host) {
            uint16_t port = skc["port"] | 3000;
            app::Command cmd;
            cmd.type = app::CommandType::RunCommand;
            snprintf(cmd.a, sizeof(cmd.a), "sk %s %u", host, (unsigned)port);
            app::post(cmd, 50);
            return;
        }
    }
    JsonVariantConst dev = doc["device"];
    if (!dev.isNull()) {
        const char *id = dev["id"];
        if (id && *id) {
            app::Command cmd;
            cmd.type = app::CommandType::RunCommand;
            snprintf(cmd.a, sizeof(cmd.a), "id %s", id);
            app::post(cmd, 50);
            return;
        }
    }
    const char *view = doc["view"].as<const char *>();
    if (view) {
        app::Command cmd;
        cmd.type = app::CommandType::ShowScreen;
        strncpy(cmd.a, view, sizeof(cmd.a) - 1);
        app::post(cmd, 50);
        return;
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
        String body;
        bool have = layout::copy_last_json(body);
        if (have && body.length() && body.length() <= 512) {
            c->setValue((const uint8_t *)body.c_str(), body.length());
            return;
        }
        // Fall back to summary: total size + screen list
        JsonDocument doc(&espdisp::psram_json);
        doc["truncated"] = true;
        doc["size"] = (uint32_t)(have ? body.length() : 0);
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
        // Copy into a PSRAM blob and queue for the UI task. BLE callback
        // must not call layout::apply_json directly (touches LVGL state
        // indirectly via the screen renderer reading it).
        void *blob = heap_caps_malloc(data.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!blob) {
            net::logf("[bleconfig] configuration write: blob alloc failed");
            return;
        }
        memcpy(blob, data.data(), data.size());
        app::Command cmd;
        cmd.type = app::CommandType::ApplyLayout;
        cmd.blob = blob;
        cmd.blob_len = data.size();
        if (!app::post(cmd, 50)) {
            heap_caps_free(blob);
            net::logf("[bleconfig] configuration write: queue full");
        }
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
        String body;
        if (layout::copy_last_json(body) && body.length())
            s_config->setValue((const uint8_t *)body.c_str(), body.length());
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
        String body;
        bool have = layout::copy_last_json(body);
        if (have && body.length() && body.length() <= 512) {
            s_config->setValue((const uint8_t *)body.c_str(), body.length());
            s_config->notify();
        }
        // For layouts > 512 B, the read callback returns a summary; no notify
        // to avoid spamming the NimBLE "value exceeds max" error.
    }
}

}  // namespace bleconfig
