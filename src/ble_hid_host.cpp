#include "ble_hid_host.h"

// Whole body gated: the feature exists only on the touch-display board envs
// (-D YEYBOATS_BLE_HID_HOST) and only when NimBLE is compiled in. Every other
// env (knob, harness, idf5, release) links the no-op stubs at the bottom.
#if defined(YEYBOATS_BLE_HID_HOST) && !defined(YEYBOATS_DISABLE_BLE)

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_events.h"
#include "hid_consumer_decode.h"
#include "net.h"
#include "storage.h"
#include "ui_data.h"

namespace ble_hid {

// ---- state -----------------------------------------------------------------
// All connect/scan work is serialized on s_task; the console handler and the
// NimBLE callbacks only flip flags and xTaskNotifyGive. Keep every struct here
// small — the notify callback runs on the ~4 KB NimBLE host task.

static TaskHandle_t s_task = nullptr;
static NimBLEClient *s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_pair_requested = false;
static volatile bool s_forget_requested = false;
static char s_peer_addr[18] = {0};  // "aa:bb:cc:dd:ee:ff"
static uint8_t s_peer_type = 0;     // BLE_ADDR_PUBLIC / _RANDOM
static uint32_t s_backoff_ms = 5000;
static constexpr uint32_t BACKOFF_MIN_MS = 5000;
static constexpr uint32_t BACKOFF_MAX_MS = 60000;
static volatile uint32_t s_keys_handled = 0;
static volatile uint32_t s_reconnects = 0;

// Per-action repeat suppression (150 ms). Indexed by hid_decode::Action.
static constexpr uint32_t REPEAT_MS = 150;
static uint32_t s_last_action_ms[(size_t)hid_decode::Action::Count] = {0};

static constexpr int BRIGHTNESS_STEP = 24;
static constexpr int BRIGHTNESS_MIN = 10;  // never let the remote black out the panel

static void wake_worker() {
    if (s_task) xTaskNotifyGive(s_task);
}

static bool have_peer() {
    return s_peer_addr[0] != 0;
}

static void load_peer() {
    storage::Namespace prefs("hidhost", true);
    std::string addr = prefs.get_string("addr", "");
    strlcpy(s_peer_addr, addr.c_str(), sizeof(s_peer_addr));
    s_peer_type = prefs.get_u8("type", 0);
}

static void save_peer(const char *addr, uint8_t type) {
    storage::Namespace prefs("hidhost", false);
    prefs.put_string("addr", addr);
    prefs.put_u8("type", type);
}

static void clear_peer_nvs() {
    storage::Namespace prefs("hidhost", false);
    prefs.remove("addr");
    prefs.remove("type");
}

// ---- input path -------------------------------------------------------------
// Runs on the NimBLE host task (~4 KB stack): decode the few report bytes via
// the pure module and app::post — nothing heavier.

static void post_action(hid_decode::Action act) {
    app::Command cmd;
    switch (act) {
    case hid_decode::Action::BrightnessUp:
    case hid_decode::Action::BrightnessDown: {
        int cur = (int)ui::brightness();
        int next =
            cur + (act == hid_decode::Action::BrightnessUp ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP);
        if (next < BRIGHTNESS_MIN) next = BRIGHTNESS_MIN;
        if (next > 255) next = 255;
        cmd.type = app::CommandType::SetBrightness;
        cmd.i = next;
        break;
    }
    case hid_decode::Action::ScreenNext:
        cmd.type = app::CommandType::ShowScreen;
        strlcpy(cmd.a, "next", sizeof(cmd.a));
        break;
    case hid_decode::Action::ScreenPrev:
        cmd.type = app::CommandType::ShowScreen;
        strlcpy(cmd.a, "prev", sizeof(cmd.a));
        break;
    case hid_decode::Action::Select:
        // Play/Pause, Menu-Pick, and keyboard Enter all decode to Select;
        // app_events.cpp's pump() dismisses a MIDL zoom overlay if one is
        // active, else toggles to/from the dashboard.
        cmd.type = app::CommandType::Select;
        break;
    default:
        return;
    }
    if (app::post(cmd, 0)) {
        s_keys_handled = s_keys_handled + 1;
    }
}

static void on_input_report(NimBLERemoteCharacteristic *, uint8_t *data, size_t len,
                            bool /*isNotify*/) {
    hid_decode::Action acts[4];
    size_t n = hid_decode::decode_actions(data, len, acts, 4);
    uint32_t now = millis();
    for (size_t i = 0; i < n; ++i) {
        size_t slot = (size_t)acts[i];
        if (slot >= (size_t)hid_decode::Action::Count) continue;
        // Key-repeat guard: ignore the same action within 150 ms.
        if (s_last_action_ms[slot] && (now - s_last_action_ms[slot]) < REPEAT_MS) continue;
        s_last_action_ms[slot] = now;
        post_action(acts[i]);
    }
}

// ---- link management (worker task only) --------------------------------------

class ClientCb : public NimBLEClientCallbacks {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    void onConnect(NimBLEClient *) override {}
    void onDisconnect(NimBLEClient *, int reason) override {
        s_connected = false;
        s_backoff_ms = BACKOFF_MIN_MS;
        net::logf("[hid] remote disconnected (reason %d)", reason);
        wake_worker();
    }
#else
    void onConnect(NimBLEClient *) override {}
    void onDisconnect(NimBLEClient *) override {
        s_connected = false;
        s_backoff_ms = BACKOFF_MIN_MS;
        net::logf("[hid] remote disconnected");
        wake_worker();
    }
#endif
};

static ClientCb s_client_cb;

static void drop_client() {
    if (!s_client) return;
    if (s_client->isConnected()) s_client->disconnect();
    NimBLEDevice::deleteClient(s_client);
    s_client = nullptr;
    s_connected = false;
}

// Connect to the stored peer, bond, and subscribe to every HID Input Report.
// Worker task only (blocking GATT calls).
static bool connect_peer() {
    if (!have_peer()) return false;
    drop_client();
    s_client = NimBLEDevice::createClient();
    if (!s_client) return false;
    s_client->setClientCallbacks(&s_client_cb, false);
    s_client->setConnectTimeout(8);  // seconds

    NimBLEAddress addr(std::string(s_peer_addr), s_peer_type);
    if (!s_client->connect(addr)) {
        net::logf("[hid] connect %s failed", s_peer_addr);
        drop_client();
        return false;
    }

    // Bond (encrypt the link). HID remotes require encryption for their input
    // reports; setSecurityAuth(bond=true, mitm=false, sc=true) is configured
    // globally in bleSetup(). Best-effort: some clones skip pairing.
    if (!s_client->secureConnection()) {
        net::logf("[hid] secureConnection failed (continuing unencrypted)");
    }

    NimBLERemoteService *svc = s_client->getService(NimBLEUUID((uint16_t)0x1812));
    if (!svc) {
        net::logf("[hid] peer %s has no HID service", s_peer_addr);
        drop_client();
        return false;
    }

    // Report Map (0x2A4B): best-effort read, logged for diagnostics only —
    // decoding is heuristic (see hid_consumer_decode.h).
    NimBLERemoteCharacteristic *rmap = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A4B));
    if (rmap && rmap->canRead()) {
        NimBLEAttValue v = rmap->readValue();
        net::logf("[hid] report map: %u bytes", (unsigned)v.length());
    }

    // Subscribe to every Input Report (0x2A4D) that can notify.
    int subs = 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    const std::vector<NimBLERemoteCharacteristic *> &chars = svc->getCharacteristics(true);
    for (NimBLERemoteCharacteristic *ch : chars) {
#else
    std::vector<NimBLERemoteCharacteristic *> *pchars = svc->getCharacteristics(true);
    if (!pchars) {
        drop_client();
        return false;
    }
    for (NimBLERemoteCharacteristic *ch : *pchars) {
#endif
        if (ch->getUUID() != NimBLEUUID((uint16_t)0x2A4D)) continue;
        if (!ch->canNotify()) continue;
        if (ch->subscribe(true, on_input_report)) ++subs;
    }
    if (subs == 0) {
        net::logf("[hid] no notifiable input reports on %s", s_peer_addr);
        drop_client();
        return false;
    }
    s_connected = true;
    s_reconnects = s_reconnects + 1;
    net::logf("[hid] connected to %s (%d input report%s)", s_peer_addr, subs, subs == 1 ? "" : "s");
    return true;
}

// Active scan (~15 s) for HID peripherals (service 0x1812); bond with the
// strongest. Worker task only.
static void do_pair() {
    net::logf("[hid] pairing: scanning 15 s for HID remotes (put yours in pairing mode)");
    drop_client();

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) {
        net::logf("[hid] scan unavailable");
        return;
    }
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    NimBLEScanResults results = scan->getResults(15 * 1000, false);
#else
    NimBLEScanResults results = scan->start(15, false);
#endif

    NimBLEUUID hid_uuid((uint16_t)0x1812);
    int best = -1;
    int best_rssi = -1000;
    int count = results.getCount();
    for (int i = 0; i < count; ++i) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        if (!dev.isAdvertisingService(hid_uuid)) continue;
        int rssi = dev.getRSSI();
        net::logf("[hid]   candidate %s '%s' rssi=%d", dev.getAddress().toString().c_str(),
                  dev.getName().c_str(), rssi);
        if (rssi > best_rssi) {
            best_rssi = rssi;
            best = i;
        }
    }
    if (best < 0) {
        net::logf("[hid] pairing: no HID (0x1812) device found");
        scan->clearResults();
        return;
    }

    NimBLEAdvertisedDevice dev = results.getDevice(best);
    char addr[18];
    strlcpy(addr, dev.getAddress().toString().c_str(), sizeof(addr));
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    uint8_t type = dev.getAddress().getType();
#else
    uint8_t type = dev.getAddressType();
#endif
    scan->clearResults();

    strlcpy(s_peer_addr, addr, sizeof(s_peer_addr));
    s_peer_type = type;
    if (connect_peer()) {
        save_peer(addr, type);
        s_backoff_ms = BACKOFF_MIN_MS;
        net::logf("[hid] paired + stored %s (type %u)", addr, (unsigned)type);
    } else {
        net::logf("[hid] pairing connect failed; nothing stored");
        s_peer_addr[0] = 0;
        load_peer();  // restore any previously stored peer
    }
}

static void do_forget() {
    net::logf("[hid] forgetting %s", have_peer() ? s_peer_addr : "(none)");
    drop_client();
    if (have_peer()) {
        NimBLEAddress addr(std::string(s_peer_addr), s_peer_type);
        NimBLEDevice::deleteBond(addr);
    }
    s_peer_addr[0] = 0;
    s_peer_type = 0;
    clear_peer_nvs();
    s_backoff_ms = BACKOFF_MIN_MS;
}

// Reconnect worker. 4 KB stack like ble_advertising_watchdog-scale helpers;
// scan results / GATT discovery buffers live on the NimBLE host task and the
// heap, not here.
static void hid_host_task(void *) {
    load_peer();
    // Give WiFi/BLE bring-up a moment before the first connect attempt.
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        if (s_forget_requested) {
            s_forget_requested = false;
            do_forget();
        }
        if (s_pair_requested) {
            s_pair_requested = false;
            do_pair();
        }
        if (!s_connected && have_peer() && !s_pair_requested && !s_forget_requested) {
            if (connect_peer()) {
                s_backoff_ms = BACKOFF_MIN_MS;
            } else {
                s_backoff_ms = s_backoff_ms * 2;
                if (s_backoff_ms > BACKOFF_MAX_MS) s_backoff_ms = BACKOFF_MAX_MS;
                net::logf("[hid] reconnect in %u s", (unsigned)(s_backoff_ms / 1000));
            }
        }
        // Connected (or nothing to do): sleep until a callback/command wakes
        // us. Disconnected with a stored peer: wake up after the backoff.
        TickType_t wait =
            (!s_connected && have_peer()) ? pdMS_TO_TICKS(s_backoff_ms) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

// ---- public API ---------------------------------------------------------------

void setup() {
    if (s_task) return;
    xTaskCreatePinnedToCore(hid_host_task, "hid-host", 4096, nullptr, 1, &s_task, 0);
    net::logf("[hid] host task up (peer %s)", have_peer() ? s_peer_addr : "unpaired");
}

bool connected() {
    return s_connected;
}

bool handleSerialCommand(const String &line) {
    if (line == "hid-pair") {
        s_pair_requested = true;
        wake_worker();
        net::logf("[hid] pair requested");
        return true;
    }
    if (line == "hid-forget") {
        s_forget_requested = true;
        wake_worker();
        return true;
    }
    if (line == "hid-status") {
        char peer[24];
        strlcpy(peer, s_peer_addr[0] ? s_peer_addr : "(none)", sizeof(peer));
        net::logf("[hid] peer=%s state=%s backoff=%us keys=%u links=%u", peer,
                  s_connected      ? "connected"
                  : s_peer_addr[0] ? "reconnecting"
                                   : "unpaired",
                  (unsigned)(s_backoff_ms / 1000), (unsigned)s_keys_handled,
                  (unsigned)s_reconnects);
        return true;
    }
    return false;
}

}  // namespace ble_hid

#else  // !YEYBOATS_BLE_HID_HOST || YEYBOATS_DISABLE_BLE

// No-op stubs so net.cpp links identically on envs without the feature
// (knob, harness, idf5, release). Signatures match include/ble_hid_host.h.
namespace ble_hid {
void setup() {
}
bool handleSerialCommand(const String &) {
    return false;
}
bool connected() {
    return false;
}
}  // namespace ble_hid

#endif  // YEYBOATS_BLE_HID_HOST
