#include "ble_config.h"
#include "net.h"
#include "psram_json.h"
#include "signalk.h"
#include "layout_loader.h"
#include "app_events.h"
#include "proto_target.h"
#include "proto/proto.h"
#include "proto/records_generated.h"

#include <ArduinoJson.h>
// BLE (NimBLE-Arduino) is compiled out under YEYBOATS_DISABLE_BLE for the
// esp-idf 5.x toolchain where the 1.4 library is absent. When the flag is
// undefined this whole file is byte-for-byte unchanged. When it is defined,
// the NimBLE body below vanishes and harmless no-op stubs take its place.
#ifndef YEYBOATS_DISABLE_BLE
#include <NimBLEDevice.h>
#include "storage.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

// NimBLE 1.4 (Arduino 2.x) vs NimBLE 2.x (Arduino 3.x) characteristic-callback
// signatures differ: 2.x adds a trailing `NimBLEConnInfo&`. These macros keep
// the GATT callback declarations identical across both libraries so the
// behavior below is shared. (Same ESP_ARDUINO_VERSION_MAJOR>=3 split the
// project uses elsewhere for Arduino-3.x API changes.)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define BLE_ON_READ(c) void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override
#define BLE_ON_WRITE(c) void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override
#else
#define BLE_ON_READ(c) void onRead(NimBLECharacteristic *c) override
#define BLE_ON_WRITE(c) void onWrite(NimBLECharacteristic *c) override
#endif

#include "wifi_scan_json.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace bleconfig {

static NimBLECharacteristic *s_conn = nullptr;
static NimBLECharacteristic *s_config = nullptr;
static NimBLECharacteristic *s_wifiscan = nullptr;

// Notify a characteristic only if a central has actually subscribed (CCCD
// written). Emitting a notification for an unsubscribed characteristic is an
// ATT protocol violation that strict centrals (CoreBluetooth) answer by
// dropping the link.
//   - NimBLE 1.4: query getSubscribedCount() and guard the notify().
//   - NimBLE 2.x: getSubscribedCount() was removed; notify() already consults
//     each peer's CCCD in the host (ble_gattc_notify_custom) and transmits only
//     to subscribed peers, so an unguarded notify() is safe (it no-ops for
//     unsubscribed peers rather than violating ATT).
static inline void ble_notify_if_subscribed(NimBLECharacteristic *c) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    c->notify();
#else
    if (c->getSubscribedCount() > 0) c->notify();
#endif
}

// Control GATT (espdisp control protocol over BLE).
static NimBLECharacteristic *s_ctrl_device = nullptr;
static NimBLECharacteristic *s_ctrl_control = nullptr;
static NimBLECharacteristic *s_ctrl_state = nullptr;
static NimBLECharacteristic *s_ctrl_resp = nullptr;

// Build the JSON document returned on CONNECTION reads.
static String connectionJson() {
    JsonDocument doc(&yeyboats::psram_json);
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
    JsonDocument doc(&yeyboats::psram_json);
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
            // NET-2: route through the console `wifi` command on the net
            // worker -> net::joinWifi (persist + live join, no reboot). The
            // ssid is quoted so names with spaces survive the line parser.
            app::Command cmd;
            cmd.type = app::CommandType::RunCommand;
            snprintf(cmd.a, sizeof(cmd.a), "wifi \"%s\" %s", ssid, pass);
            app::post_net(cmd, 50);
            net::logf("[bleconfig] wifi join queued (no reboot)");
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
    BLE_ON_READ(c) {
        String j = connectionJson();
        c->setValue((const uint8_t *)j.c_str(), j.length());
    }
    BLE_ON_WRITE(c) { applyConnectionWrite(std::string(c->getValue())); }
};

// ---- WIFISCAN characteristic (BLE-3) ----------------------------------------
// A write of "scan" wakes the worker task below; the GATT callback itself
// never touches the WiFi driver. The worker kicks WiFi.scanNetworks(async),
// polls completion OUTSIDE any GATT callback, then publishes the JSON result
// (sorted strongest-first, truncated to the 512-byte attribute cap) on the
// characteristic value + a notify for subscribed centrals.

static TaskHandle_t s_wifiscan_task = nullptr;

static void wifi_scan_worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
            WiFi.scanNetworks(true /* async */, true /* show hidden */);
        }
        int n = WIFI_SCAN_RUNNING;
        uint32_t t0 = millis();
        while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() - t0 < 15000) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        // Scratch buffers off this 4 KB task stack (large-local memory trap).
        // The worker is the only writer, so function-statics are race-free.
        static wifi_scan_json::Ap aps[wifi_scan_json::MAX_APS];
        memset(aps, 0, sizeof(aps));
        size_t cnt = 0;
        for (int i = 0; n > 0 && i < n && cnt < wifi_scan_json::MAX_APS; ++i) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;  // hidden networks: nothing to show
            strlcpy(aps[cnt].ssid, ssid.c_str(), sizeof(aps[cnt].ssid));
            aps[cnt].rssi = (int16_t)WiFi.RSSI(i);
            aps[cnt].sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            ++cnt;
        }
        if (n >= 0) WiFi.scanDelete();
        static char json[512];
        size_t len = wifi_scan_json::to_json(aps, cnt, json, sizeof(json));
        if (s_wifiscan) {
            s_wifiscan->setValue((const uint8_t *)json, len);
            ble_notify_if_subscribed(s_wifiscan);
        }
        net::logf("[bleconfig] wifi scan -> %d networks, %u bytes", n < 0 ? 0 : n, (unsigned)len);
    }
}

class WifiScanCb : public NimBLECharacteristicCallbacks {
    BLE_ON_WRITE(c) {
        std::string v(c->getValue());
        if (v != "scan") {
            net::logf("[bleconfig] wifiscan write ignored (expected \"scan\")");
            return;
        }
        static const char kRunning[] = "{\"running\":true}";
        c->setValue((const uint8_t *)kRunning, sizeof(kRunning) - 1);
        if (s_wifiscan_task) xTaskNotifyGive(s_wifiscan_task);
    }
};

class ConfigCb : public NimBLECharacteristicCallbacks {
    BLE_ON_READ(c) {
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
        JsonDocument doc(&yeyboats::psram_json);
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
    BLE_ON_WRITE(c) {
        std::string data(c->getValue());
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

// ---- Control GATT service (espdisp control protocol over BLE) ----
//
// This is the BLE fallback for the HTTP /api/p2p/* path. It reuses the SAME
// proto_target::handle_* handlers (one session/auth/version code path for both
// transports) and the SAME generated (de)serializers (proto::from_json /
// proto::to_json). No session or auth logic is duplicated here.
//
// Readback note: a BLE WRITE has no response payload, so after a CONTROL write
// we stash the ack JSON on the RESP characteristic (READ + NOTIFY) and refresh
// STATE; a central reads the attach sessionId back from RESP (and/or its notify).

// Build a DeviceRecord JSON, summarized to stay under the 512-byte BLE
// attribute cap (trap #5). DeviceRecord can carry up to 16 ViewRefs (~ a few
// KiB serialized); we cap the views list to the first few and flag the
// truncation. The full views list is always available over IP (GET
// /api/p2p/device), which has no such cap.
static String controlDeviceJson() {
    // DeviceRecord is ~1.5 KB (ViewRef[16] + transports[16] + fixed strings).
    // The NimBLE host-task stack is small (~4 KB); a stack-local of this size
    // overflows it and reboots the device (same class as the project's "large
    // stack temporary" trap). GATT callbacks run serially on the one NimBLE
    // host task, so a function-static scratch is safe (no re-entrancy).
    static proto::DeviceRecord r;
    memset(&r, 0, sizeof(r));
    proto_target::fill_device_record(r);

    // Cap views so the serialized record fits the 512-byte GATT read. Each
    // ViewRef is id+title; ~4 views keeps us comfortably under the cap even
    // with long ids. Mark "views" as truncated when we drop any.
    constexpr int kBleMaxViews = 4;
    bool truncated = false;
    if (r.views_count > kBleMaxViews) {
        r.views_count = kBleMaxViews;
        truncated = true;
    }

    JsonDocument doc(&yeyboats::psram_json);
    JsonObject o = doc.to<JsonObject>();
    proto::to_json(o, r);
    if (truncated) o["viewsTruncated"] = true;  // hint: fetch full list over IP

    String out;
    serializeJson(doc, out);

    // Byte guard: even with 4 views, long ids/titles can exceed the 512-byte
    // GATT attribute cap. Drop views entirely and re-serialize; zero views is
    // ~200 bytes so comfortably fits. viewsTruncated stays true so the central
    // knows to fetch the full list over IP (GET /api/p2p/device).
    if (out.length() > 512) {
        r.views_count = 0;
        doc.clear();
        JsonObject o2 = doc.to<JsonObject>();
        proto::to_json(o2, r);
        o2["viewsTruncated"] = true;
        out = "";
        serializeJson(doc, out);
    }

    // Hard cap: should never trigger after empty-views re-serialize, but
    // prevents NimBLE from receiving an over-cap buffer in any edge case.
    if (out.length() > 512) {
        out.remove(512);
    }

    return out;
}

// Build a ControlState JSON. ControlState holds at most kMaxSessions (8)
// Sessions; each Session is small (controllerId/name/color/lastSeen) and 8 of
// them serialize well under 512 bytes, so no summarization is required here.
static String controlStateJson() {
    // ControlState holds Session[16] (~2 KB) — too large for the NimBLE host
    // task stack. Function-static scratch (GATT callbacks are serial). See
    // controlDeviceJson() for the rationale.
    static proto::ControlState cs;
    memset(&cs, 0, sizeof(cs));
    proto_target::fill_state(cs);
    JsonDocument doc(&yeyboats::psram_json);
    proto::to_json(doc.to<JsonObject>(), cs);
    String out;
    serializeJson(doc, out);
    return out;
}

// Dispatch one inbound Control message. Reuses proto_target::handle_* exactly
// like web.cpp; returns the ack JSON to stash on RESP. Rejects incompatible
// protocol versions (mirrors the HTTP 400 incompatible_version gate).
static String dispatchControl(const std::string &data) {
    JsonDocument doc(&yeyboats::psram_json);
    if (deserializeJson(doc, data) != DeserializationError::Ok) {
        return String("{\"v\":\"1.0\",\"ok\":false,\"reason\":\"bad_json\"}");
    }
    JsonObjectConst o = doc.as<JsonObjectConst>();
    const char *t = o["t"] | "";
    const char *v = o["v"] | "";

    if (!proto::version_compatible(v)) {
        return String("{\"v\":\"1.0\",\"ok\":false,\"reason\":\"incompatible_version\"}");
    }

    JsonDocument out(&yeyboats::psram_json);

    if (strcmp(t, "attach") == 0) {
        // AttachAck embeds a DeviceRecord (~1.5 KB) — keep it off the small
        // NimBLE host-task stack. Static scratch (GATT callbacks are serial).
        static proto::Attach req;
        static proto::AttachAck ack;
        memset(&req, 0, sizeof(req));
        memset(&ack, 0, sizeof(ack));
        proto::from_json(o, req);
        proto_target::handle_attach(req, ack);
        // Drop the embedded DeviceRecord views to keep the ack within 512 B; the
        // sessionId (the thing a central needs to read back) and accepted flag
        // are the load-bearing fields. Full device record is on the DEVICE char.
        ack.device.views_count = 0;
        ack.device.transports_count = 0;
        proto::to_json(out.to<JsonObject>(), ack);
    } else if (strcmp(t, "switch") == 0) {
        proto::Switch req;
        proto::from_json(o, req);
        proto::SwitchAck ack;
        proto_target::handle_switch(req, ack);
        proto::to_json(out.to<JsonObject>(), ack);
    } else if (strcmp(t, "heartbeat") == 0) {
        proto::Heartbeat req;
        proto::from_json(o, req);
        bool ok = proto_target::handle_heartbeat(req.sessionId);
        proto::HeartbeatAck ack;
        strncpy(ack.v, "1.0", sizeof(ack.v) - 1);
        ack.ok = ok;
        ack.ttlMs = proto::kDefaultTtlMs;
        proto::to_json(out.to<JsonObject>(), ack);
    } else if (strcmp(t, "detach") == 0) {
        proto::Detach req;
        proto::from_json(o, req);
        bool ok = proto_target::handle_detach(req.sessionId);
        JsonObject ro = out.to<JsonObject>();
        ro["v"] = "1.0";
        ro["t"] = "detachAck";
        ro["ok"] = ok;
    } else {
        JsonObject ro = out.to<JsonObject>();
        ro["v"] = "1.0";
        ro["ok"] = false;
        ro["reason"] = "unknown_type";
    }

    String payload;
    serializeJson(out, payload);
    return payload;
}

class CtrlDeviceCb : public NimBLECharacteristicCallbacks {
    BLE_ON_READ(c) {
        String j = controlDeviceJson();
        c->setValue((const uint8_t *)j.c_str(), j.length());
    }
};

class CtrlStateCb : public NimBLECharacteristicCallbacks {
    BLE_ON_READ(c) {
        String j = controlStateJson();
        c->setValue((const uint8_t *)j.c_str(), j.length());
    }
};

class CtrlControlCb : public NimBLECharacteristicCallbacks {
    BLE_ON_WRITE(c) {
        std::string data(c->getValue());
        if (data.empty()) return;
        if (data.size() > 512) {
            net::logf("[bleconfig] control write: rejected (size > 512)");
            return;
        }
        // proto_target::handle_* is thread-safe (mutex-guarded) and posts UI
        // work via app::Command, so it is safe to run from the BLE callback.
        String ack = dispatchControl(data);

        // Stash the ack on RESP so the central can read back the sessionId (a
        // BLE WRITE returns no body), and refresh STATE so the next read reflects
        // the new session set. Notify ONLY when the client has actually subscribed
        // (CCCD written) — emitting a notification for an unsubscribed
        // characteristic is an ATT protocol violation that CoreBluetooth (and
        // other strict centrals) answer by dropping the link. setValue alone is
        // enough for a central that reads RESP/STATE back (the common path);
        // subscribers additionally get the notify.
        if (s_ctrl_resp) {
            s_ctrl_resp->setValue((const uint8_t *)ack.c_str(), ack.length());
            ble_notify_if_subscribed(s_ctrl_resp);
        }
        if (s_ctrl_state) {
            String st = controlStateJson();
            s_ctrl_state->setValue((const uint8_t *)st.c_str(), st.length());
            ble_notify_if_subscribed(s_ctrl_state);
        }
    }
};

static void setupControlService(NimBLEServer *server) {
    NimBLEService *svc = server->createService(CTRL_SERVICE_UUID);

    s_ctrl_device = svc->createCharacteristic(CTRL_DEVICE_UUID, NIMBLE_PROPERTY::READ);
    s_ctrl_device->setCallbacks(new CtrlDeviceCb());
    {
        String j = controlDeviceJson();
        s_ctrl_device->setValue((const uint8_t *)j.c_str(), j.length());
    }

    s_ctrl_control = svc->createCharacteristic(CTRL_CONTROL_UUID,
                                               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    s_ctrl_control->setCallbacks(new CtrlControlCb());

    s_ctrl_state =
        svc->createCharacteristic(CTRL_STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_ctrl_state->setCallbacks(new CtrlStateCb());
    {
        String j = controlStateJson();
        s_ctrl_state->setValue((const uint8_t *)j.c_str(), j.length());
    }

    s_ctrl_resp =
        svc->createCharacteristic(CTRL_RESP_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    {
        static const char kInit[] = "{\"v\":\"1.0\",\"t\":\"ready\"}";
        s_ctrl_resp->setValue((const uint8_t *)kInit, sizeof(kInit) - 1);
    }

    svc->start();

    // Advertise the Control service UUID so a central can find espdisp targets
    // by it. The primary advertising packet's service list can be tight (31-byte
    // budget); put the Control UUID in the scan response to stay within budget.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(CTRL_SERVICE_UUID);

    net::logf("[bleconfig] Control GATT service registered (%s)", CTRL_SERVICE_UUID);
}

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

    // WIFISCAN (BLE-3): write "scan" -> async WiFi scan; result readable +
    // notified as a JSON array. The scan itself runs on wifi_scan_worker.
    s_wifiscan = svc->createCharacteristic(
        WIFISCAN_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    s_wifiscan->setCallbacks(new WifiScanCb());
    {
        static const char kEmpty[] = "[]";
        s_wifiscan->setValue((const uint8_t *)kEmpty, sizeof(kEmpty) - 1);
    }
    if (!s_wifiscan_task) {
        xTaskCreatePinnedToCore(wifi_scan_worker, "ble-scan", 4096, nullptr, 1, &s_wifiscan_task,
                                0);
    }

    svc->start();

    // Advertise the new service alongside the existing Nordic UART.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);

    net::logf("[bleconfig] GATT service registered (%s)", SERVICE_UUID);

    // Add the espdisp Control GATT service (BLE fallback for /api/p2p/*).
    setupControlService(server);
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

#else  // YEYBOATS_DISABLE_BLE

// No-op stubs so callers (net.cpp, layout_loader.cpp) still link when NimBLE
// is compiled out. Signatures match include/ble_config.h exactly.
namespace bleconfig {
void setup() {
}
void notifyAll() {
}
}  // namespace bleconfig

#endif  // YEYBOATS_DISABLE_BLE
