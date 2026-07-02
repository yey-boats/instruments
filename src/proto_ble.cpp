// Controller-side, on-demand BLE central for the espdisp control protocol.
// See proto_ble.h for the on-demand contract (scan -> connect-one -> control ->
// disconnect -> deleteClient, never an idle connection). This file is in the
// build_src_filter of the display envs too, but its whole body is gated on
// YEYBOATS_BLE_CENTRAL so display-only builds compile it to nothing — the
// display stays peripheral-only and never links NimBLE's central API.
#include "proto_ble.h"

#if defined(YEYBOATS_BLE_CENTRAL)

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <string.h>

#include "ble_config.h"  // CTRL_* UUIDs (Phase 4.1) — reuse, do not redefine
#include "net.h"

namespace proto_ble {

// Reuse the Control service/characteristic UUIDs defined by the target side
// (Phase 4.1) — do not redefine them. They live in namespace bleconfig.
using bleconfig::CTRL_CONTROL_UUID;
using bleconfig::CTRL_DEVICE_UUID;
using bleconfig::CTRL_RESP_UUID;
using bleconfig::CTRL_SERVICE_UUID;

namespace {

// Worker-serialized single-connection guard. The knob drives BLE control from
// exactly one task (the manager worker), so a plain bool is enough to assert
// that contract in debug; it also blocks a re-entrant scan/connect if the model
// is ever violated. Not a substitute for task discipline — see the header.
volatile bool s_busy = false;

struct BusyGuard {
    bool acquired = false;
    BusyGuard() {
        if (!s_busy) {
            s_busy = true;
            acquired = true;
        }
    }
    ~BusyGuard() {
        if (acquired) s_busy = false;
    }
};

// RAII for the outbound client: guarantees disconnect() + deleteClient() on
// EVERY exit path (success, missing service, parse failure, early return,
// exception). This is the core of the on-demand discipline — the client is
// never left connected and never leaked.
struct ClientGuard {
    NimBLEClient *c = nullptr;
    explicit ClientGuard(NimBLEClient *client) : c(client) {}
    ~ClientGuard() {
        if (!c) return;
        if (c->isConnected()) c->disconnect();
        NimBLEDevice::deleteClient(c);
        c = nullptr;
    }
    NimBLEClient *operator->() const { return c; }
    explicit operator bool() const { return c != nullptr; }
};

// Write one serialized protocol message to the CONTROL characteristic.
// setValue/writeValue use the explicit (uint8_t*, len) overload (memory trap
// #4: never the bare-C-string overload, which can resolve to a 4-byte store).
bool write_control(NimBLERemoteService *svc, const String &payload) {
    NimBLERemoteCharacteristic *ctrl = svc->getCharacteristic(CTRL_CONTROL_UUID);
    if (!ctrl || !ctrl->canWrite()) return false;
    return ctrl->writeValue((const uint8_t *)payload.c_str(), payload.length(), true);
}

// Serialize a generated request struct to a JSON String.
template <typename T> String to_payload(const T &msg) {
    JsonDocument doc;
    proto::to_json(doc.to<JsonObject>(), msg);
    String out;
    serializeJson(doc, out);
    return out;
}

// Read the RESP characteristic (where the target stashes the last ack JSON
// after a CONTROL write — a BLE write returns no body) and from_json() it into
// `ack`. Returns false if RESP is unreadable or the body does not parse.
template <typename Ack> bool read_resp(NimBLERemoteService *svc, Ack &ack) {
    NimBLERemoteCharacteristic *resp = svc->getCharacteristic(CTRL_RESP_UUID);
    if (!resp || !resp->canRead()) return false;
    NimBLEAttValue val = resp->readValue();
    JsonDocument doc;
    if (deserializeJson(doc, val.c_str(), val.length()) != DeserializationError::Ok) return false;
    from_json(doc.as<JsonObjectConst>(), ack);
    return true;
}

}  // namespace

int scan(uint32_t timeout_ms, PeerCallback cb, void *ctx) {
    if (!cb) return -1;
    BusyGuard busy;
    if (!busy.acquired) return -1;  // a connect/scan is already in flight

    NimBLEScan *scanner = NimBLEDevice::getScan();
    if (!scanner) return -1;
    scanner->setActiveScan(true);  // need the scan response for name + mfg data
    scanner->setInterval(100);
    scanner->setWindow(80);

    // Synchronous scan: blocks for the window, returns the collected results.
    NimBLEScanResults results = scanner->start(timeout_ms / 1000 ? timeout_ms / 1000 : 1, false);

    NimBLEUUID ctrl_svc(CTRL_SERVICE_UUID);
    int n = 0;
    int count = results.getCount();
    for (int i = 0; i < count; ++i) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        if (!dev.isAdvertisingService(ctrl_svc)) continue;

        Peer p;  // small POD, fits the worker stack — no large temporaries
        std::string name = dev.getName();
        strncpy(p.device_id, name.c_str(), sizeof(p.device_id) - 1);
        std::string addr = dev.getAddress().toString();
        strncpy(p.addr, addr.c_str(), sizeof(p.addr) - 1);
        // pv is optional: the target may carry it as ASCII manufacturer data
        // in the scan response. Treat anything non-printable as absent.
        if (dev.getManufacturerDataCount() > 0) {
            std::string mfg = dev.getManufacturerData();
            if (!mfg.empty() && mfg.size() < sizeof(p.pv) &&
                mfg.find_first_not_of("0123456789.") == std::string::npos) {
                strncpy(p.pv, mfg.c_str(), sizeof(p.pv) - 1);
            }
        }
        cb(p, ctx);
        ++n;
    }
    // Drop NimBLE's scan cache so results do not accumulate across scans.
    scanner->clearResults();
    return n;
}

bool switch_on_peer(const char *addr, const proto::Attach &a, const proto::Switch &sw_template) {
    if (!addr || !*addr) return false;
    BusyGuard busy;
    if (!busy.acquired) return false;  // single connection at a time

    ClientGuard client(NimBLEDevice::createClient());
    if (!client) return false;
    client->setConnectTimeout(6);  // seconds

    NimBLEAddress peer(addr);
    if (!client->connect(peer)) return false;  // ClientGuard tears down

    NimBLERemoteService *svc = client->getService(CTRL_SERVICE_UUID);
    if (!svc) return false;  // missing service -> ClientGuard tears down

    // attach -> read AttachAck (for the sessionId)
    if (!write_control(svc, to_payload(a))) return false;
    // AttachAck embeds a DeviceRecord (~1.5 KB) — keep it off the worker-task
    // stack per the CLAUDE.md large-struct trap. BusyGuard makes central ops
    // single-flight, so a function-static scratch is race-free.
    static proto::AttachAck ack;
    memset(&ack, 0, sizeof(ack));
    if (!read_resp(svc, ack) || !ack.accepted || !ack.sessionId[0]) return false;

    // switch (carry the session) -> read SwitchAck
    proto::Switch sw = sw_template;
    strncpy(sw.sessionId, ack.sessionId, sizeof(sw.sessionId) - 1);
    bool ok = false;
    if (write_control(svc, to_payload(sw))) {
        proto::SwitchAck sa{};
        ok = read_resp(svc, sa) && sa.ok;
    }

    // detach (best-effort): release the session so the target's colored frame
    // clears immediately rather than waiting for the TTL reap. Failure here
    // does not change the switch result.
    proto::Detach d{};
    strncpy(d.v, "1.0", sizeof(d.v) - 1);
    strncpy(d.sessionId, ack.sessionId, sizeof(d.sessionId) - 1);
    write_control(svc, to_payload(d));

    return ok;
    // ClientGuard destructor: disconnect() + deleteClient() — always.
}

bool get_device_on_peer(const char *addr, proto::DeviceRecord &out) {
    if (!addr || !*addr) return false;
    BusyGuard busy;
    if (!busy.acquired) return false;

    ClientGuard client(NimBLEDevice::createClient());
    if (!client) return false;
    client->setConnectTimeout(6);

    NimBLEAddress peer(addr);
    if (!client->connect(peer)) return false;

    NimBLERemoteService *svc = client->getService(CTRL_SERVICE_UUID);
    if (!svc) return false;

    NimBLERemoteCharacteristic *dev = svc->getCharacteristic(CTRL_DEVICE_UUID);
    if (!dev || !dev->canRead()) return false;
    NimBLEAttValue val = dev->readValue();
    JsonDocument doc;
    if (deserializeJson(doc, val.c_str(), val.length()) != DeserializationError::Ok) return false;
    from_json(doc.as<JsonObjectConst>(), out);
    return true;
    // ClientGuard destructor: disconnect() + deleteClient() — always.
}

}  // namespace proto_ble

#endif  // YEYBOATS_BLE_CENTRAL
