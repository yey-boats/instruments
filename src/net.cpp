#include "net.h"

#include <Arduino.h>
#include "storage.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <freertos/semphr.h>

#include "secrets.h"
#include "signalk.h"
#include "layout_loader.h"
#include "ble_config.h"
#include "wifi_store.h"
#include "app_events.h"
#include "source_nmea_wifi.h"
#include "source_nmea2000.h"
#include "boat_cli.h"
#include "board.h"
#include "input_test.h"
#include "manager.h"
#include "beeper.h"
#include "autopilot.h"
#include "device_identity.h"
#include "device_discovery.h"

namespace net {

static IPAddress broadcastAddr;
static bool ota_started = false;
static bool ap_mode = false;
static ExtraCommandHandler s_extra = nullptr;
static String s_device_id;
static volatile WifiState s_wifi_state = WifiState::Idle;
static TaskHandle_t s_wifi_task = nullptr;
static TaskHandle_t s_ble_adv_task = nullptr;
static bool s_discovery_announcements = false;
static uint32_t s_discovery_last_ms = 0;
static uint32_t s_discovery_sent = 0;
static bool s_discovery_sk_skip_logged = false;
static bool s_mdns_started = false;
static bool s_mdns_services_registered = false;
static bool s_mdns_refresh_due = false;
static uint32_t s_mdns_last_refresh_ms = 0;
static uint32_t s_mdns_seq = 0;
static SemaphoreHandle_t s_log_mtx = nullptr;
static SemaphoreHandle_t s_udp_mtx = nullptr;
static constexpr size_t LOG_RING_CAP = 96;
static LogEntry s_log_ring[LOG_RING_CAP];
static uint32_t s_log_seq = 0;
static size_t s_log_count = 0;
static char s_discovery_ip[16] = {0};
static constexpr uint32_t MDNS_REFRESH_MS = 60000;

static String format_hardware_device_id(const uint8_t mac[6]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "espdisp-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return String(buf);
}

static String raw_efuse_device_id() {
    uint64_t mac = ESP.getEfuseMac();
    if (mac == 0) return String(OTA_HOSTNAME);
    char buf[32];
    snprintf(buf, sizeof(buf), "espdisp-%012llx", (unsigned long long)(mac & 0xffffffffffffULL));
    return String(buf);
}

static String hardware_default_device_id() {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        return format_hardware_device_id(mac);
    }
    uint64_t efuse = ESP.getEfuseMac();
    if (efuse == 0) return String(OTA_HOSTNAME);
    for (uint8_t i = 0; i < 6; ++i) {
        mac[i] = (efuse >> (8 * i)) & 0xff;
    }
    return format_hardware_device_id(mac);
}

static bool is_legacy_default_device_id(const String &id, const String &raw_efuse_id) {
    return id.length() == 0 || id == OTA_HOSTNAME || id == "espdisp-device" || id == raw_efuse_id;
}

WifiState wifiState() {
    return s_wifi_state;
}

const char *wifiStateName() {
    switch (s_wifi_state) {
    case WifiState::Idle:
        return "idle";
    case WifiState::Connecting:
        return "connecting";
    case WifiState::StaUp:
        return "sta";
    case WifiState::ApSetup:
        return "ap";
    case WifiState::Failed:
        return "failed";
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
        bool inline_ok =
            (line == "ip" || line == "bench" || line == "screen" || line == "sk-status" ||
             line == "sk-dump" || line == "wifi-list" || line == "bright");
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
    // First sleep before checking - bleSetup() already called
    // startAdvertising() so an immediate retry just logs
    // "Advertising already active" noise on every boot.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!bleConnected) {
            NimBLEDevice::startAdvertising();
        }
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
    printf("[ble] advertising as %s\n", s_device_id.c_str());
    xTaskCreatePinnedToCore(ble_advertising_watchdog, "ble-adv", 3072, nullptr, 1, &s_ble_adv_task,
                            0);
}

// ---- WiFi / OTA ----
static void otaSetup() {
    ArduinoOTA.setHostname(s_device_id.c_str());
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        printf("[ota] start (%s)\n", ArduinoOTA.getCommand() == U_FLASH ? "flash" : "fs");
    });
    ArduinoOTA.onEnd([]() { puts("[ota] end"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        static unsigned int last = 0;
        unsigned int pct = (p * 100) / t;
        if (pct != last) {
            printf("[ota] %u%%\n", pct);
            last = pct;
        }
    });
    ArduinoOTA.onError([](ota_error_t e) { printf("[ota] error %d\n", e); });
    ArduinoOTA.begin();
    ota_started = true;
    printf("[ota] ready at %s.local\n", s_device_id.c_str());
}

// Try one network with a 10s timeout. Returns true on association.
static bool try_join(const char *ssid, const char *pass) {
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(s_device_id.c_str());
    printf("[wifi] trying '%s' (pass len %u)\n", ssid, (unsigned)strlen(pass ? pass : ""));
    if (pass && *pass)
        WiFi.begin(ssid, pass);
    else
        WiFi.begin(ssid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        delay(250);
        putchar('.');
    }
    putchar('\n');
    return WiFi.status() == WL_CONNECTED;
}

static void start_ap_mode() {
    puts("[wifi] starting AP mode (full reset)");
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
        puts("[wifi] softAPConfig FAILED");
    }
    // channel 1, not hidden, max 4 clients, beacon interval default.
    bool ok = WiFi.softAP("espdisp-setup", nullptr, 1, 0, 4);
    delay(500);  // allow the DHCP server task to spin up
    if (!ok) {
        puts("[wifi] softAP FAILED");
    }
    broadcastAddr = WiFi.softAPIP();
    broadcastAddr[3] = 255;
    ap_mode = true;
    printf("[wifi] AP ip=%s  mac=%s  ssid='espdisp-setup' (open)\n",
           WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
    printf("[wifi] connected stations: %d\n", WiFi.softAPgetStationNum());
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

static bool web_auth_required() {
    storage::Namespace p("web", true);
    return p.get_u8("auth", 0) != 0;
}

static device_discovery::Info discovery_info() {
    const auto &id = device_identity::get();
    board::Geometry g = board::geometry();
    device_discovery::Info info;
    info.device_id = s_device_id.c_str();
    info.board_id = id.board_id;
    info.firmware_name = id.firmware_name;
    info.firmware_version = id.firmware_version;
    String ip = ipString();
    strlcpy(s_discovery_ip, ip.c_str(), sizeof(s_discovery_ip));
    info.ip = s_discovery_ip;
    info.port = 80;
    info.display_width = g.width_px;
    info.display_height = g.height_px;
    info.web_auth_required = web_auth_required();
    return info;
}

static void register_mdns_services() {
    if (s_mdns_services_registered) return;
    device_discovery::Info info = discovery_info();
    MDNS.addService(device_discovery::MDNS_SERVICE, "tcp", info.port);
    s_mdns_services_registered = true;
}

static void refresh_mdns_device_txt(const char *reason) {
    if (!s_mdns_started) return;
    register_mdns_services();
    device_discovery::Info info = discovery_info();
    manager::Status mst = manager::status();
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "proto",
                       device_discovery::MDNS_PROTO);
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "device_id", info.device_id);
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "path", "/");
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "board", info.board_id);
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "firmware", info.firmware_name);
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "version", info.firmware_version);
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "display",
                       String(info.display_width) + "x" + String(info.display_height));
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "auth",
                       info.web_auth_required ? "basic" : "none");
    if (mst.config_version.length()) {
        MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "cfg_ver", mst.config_version);
    }
    if (mst.config_hash.length()) {
        MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "cfg_hash", mst.config_hash);
    }
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "seq", String(++s_mdns_seq));
    s_mdns_last_refresh_ms = millis();
    s_mdns_refresh_due = false;
    printf("[mdns] advertise(%s) _%s._tcp port=%u id=%s board=%s fw=%s %s display=%ux%u auth=%s "
           "seq=%lu\n",
           reason ? reason : "refresh", device_discovery::MDNS_SERVICE, info.port, info.device_id,
           info.board_id, info.firmware_name, info.firmware_version, info.display_width,
           info.display_height, info.web_auth_required ? "basic" : "none",
           (unsigned long)s_mdns_seq);
}

static bool send_discovery_packet(IPAddress target, const char *payload, size_t len,
                                  const char *label, bool log_failures) {
    bool locked = false;
    if (s_udp_mtx && xSemaphoreTake(s_udp_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        locked = true;
    }
    WiFiUDP discovery_udp;
    if (!discovery_udp.begin(0)) {
        if (locked) xSemaphoreGive(s_udp_mtx);
        if (log_failures) logf("[discovery] announce bind failed");
        return false;
    }
    if (!discovery_udp.beginPacket(target, device_discovery::DEVICE_ANNOUNCE_PORT)) {
        if (log_failures) {
            logf("[discovery] announce begin failed %s:%u (%s)", target.toString().c_str(),
                 device_discovery::DEVICE_ANNOUNCE_PORT, label ? label : "target");
        }
        discovery_udp.stop();
        if (locked) xSemaphoreGive(s_udp_mtx);
        return false;
    }
    discovery_udp.write((const uint8_t *)payload, len);
    bool ok = discovery_udp.endPacket() == 1;
    discovery_udp.stop();
    if (locked) xSemaphoreGive(s_udp_mtx);
    if (!ok && log_failures) {
        logf("[discovery] announce send failed %s:%u (%s)", target.toString().c_str(),
             device_discovery::DEVICE_ANNOUNCE_PORT, label ? label : "target");
    }
    return ok;
}

static void send_discovery_announcement() {
    if (broadcastAddr == IPAddress(0, 0, 0, 0)) return;

    String sk_host = sk::targetHost();
    if (sk_host.length()) {
        if (!s_discovery_sk_skip_logged) {
            s_discovery_sk_skip_logged = true;
            logf("[discovery] UDP announces paused; SignalK target is %s", sk_host.c_str());
        }
        return;
    }

    device_discovery::Info info = discovery_info();
    JsonDocument doc;
    device_discovery::build_announcement(doc, info);
    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        logf("[discovery] announce encode failed");
        return;
    }

    bool ok = send_discovery_packet(broadcastAddr, buf, n, "broadcast", true);
    if (ok && (++s_discovery_sent == 1 || s_discovery_sent % 12 == 0)) {
        logf("[discovery] announce %s:%u id=%s auth=%s sk=%s", broadcastAddr.toString().c_str(),
             device_discovery::DEVICE_ANNOUNCE_PORT, info.device_id,
             info.web_auth_required ? "basic" : "none", "-");
    }
}

static void start_discovery_announcements() {
    if (s_discovery_announcements) return;
    s_discovery_announcements = true;
    s_discovery_last_ms = 0;
    s_discovery_sent = 0;
    s_discovery_sk_skip_logged = false;
    logf("[discovery] announce loop started");
}

static void wifi_manager_task(void *) {
    s_wifi_state = WifiState::Connecting;
    wifi_store::load();
    wifi_store::migrate_legacy_if_any();

    if (wifi_store::count() == 0) {
        puts("[wifi] no saved networks, AP-only");
        start_ap_mode();
        s_wifi_state = WifiState::ApSetup;
        post_show_wifi_screen();
        vTaskDelete(NULL);
        return;
    }

    printf("[wifi] %u saved network%s, trying in order\n", (unsigned)wifi_store::count(),
           wifi_store::count() == 1 ? "" : "s");

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
        IPAddress mask = WiFi.subnetMask();
        broadcastAddr = ip;
        for (uint8_t i = 0; i < 4; ++i) {
            broadcastAddr[i] = ip[i] | ~mask[i];
        }
        printf("[wifi] up: ip=%s  ssid='%s'  rssi=%d\n", ip.toString().c_str(), WiFi.SSID().c_str(),
               WiFi.RSSI());
        if (MDNS.begin(s_device_id.c_str())) {
            s_mdns_started = true;
            s_mdns_services_registered = false;
            MDNS.addService("arduino", "tcp", 3232);
            refresh_mdns_device_txt("boot");
            printf("[mdns] host %s.local (espdisp + arduino)\n", s_device_id.c_str());
        }
        otaSetup();
        s_wifi_state = WifiState::StaUp;
        start_discovery_announcements();
    } else {
        puts("[wifi] all saved networks failed, fallback to AP");
        start_ap_mode();
        s_wifi_state = WifiState::ApSetup;
        post_show_wifi_screen();
    }
    vTaskDelete(NULL);
}

void setup() {
    if (!s_log_mtx) s_log_mtx = xSemaphoreCreateMutex();
    if (!s_udp_mtx) s_udp_mtx = xSemaphoreCreateMutex();
    {
        storage::Namespace prefs("net", false);
        String fallback_id = hardware_default_device_id();
        String raw_efuse_id = raw_efuse_device_id();
        String stored_id = String(prefs.get_string("device_id", "").c_str());
        if (is_legacy_default_device_id(stored_id, raw_efuse_id)) {
            s_device_id = fallback_id;
            if (stored_id.length() && stored_id != fallback_id) {
                prefs.put_string("device_id", s_device_id.c_str());
                printf("[net] migrated legacy device id '%s' -> '%s'\n", stored_id.c_str(),
                       s_device_id.c_str());
            }
        } else {
            s_device_id = stored_id;
        }
    }
    printf("[net] device id: %s\n", s_device_id.c_str());
    // BLE is independent of WiFi - bring it up immediately so the BLE
    // console is responsive even if the WiFi manager is still trying.
    bleSetup();
    // WiFi join attempts run on their own task (Phase 4). With multiple
    // saved networks and 10 s per try this could be 30+ seconds blocking
    // the main loop; making it async lets the UI render immediately.
    xTaskCreatePinnedToCore(wifi_manager_task, "wifi-mgr", 6144, nullptr, 1, &s_wifi_task, 0);
}

bool dispatchCommand(const String &line) {
    if (handleSerialCommand(line)) return true;
    if (sk::handleSerialCommand(line)) return true;
    if (nmea_wifi::handleSerialCommand(line)) return true;
    if (nmea2000::handleSerialCommand(line)) return true;
    if (boat::handleSerialCommand(line)) return true;
    if (board::handleSerialCommand(line)) return true;
    if (input_test::handleConsoleCommand(line)) return true;
    if (manager::handleSerialCommand(line)) return true;
    if (beeper::handleSerialCommand(line)) return true;
    if (autopilot::handleSerialCommand(line)) return true;
    if (layout::handleSerialCommand(line)) return true;
    if (s_extra && s_extra(line)) return true;
    return false;
}

void loop() {
    if (ota_started) ArduinoOTA.handle();
    if (s_mdns_started && wifiUp()) {
        uint32_t now = millis();
        if (s_mdns_refresh_due ||
            (int32_t)(now - s_mdns_last_refresh_ms) >= (int32_t)MDNS_REFRESH_MS) {
            refresh_mdns_device_txt(s_mdns_refresh_due ? "requested" : "periodic");
        }
    }
    if (s_discovery_announcements && wifiUp()) {
        uint32_t now = millis();
        if (s_discovery_last_ms == 0 || (int32_t)(now - s_discovery_last_ms) >= 5000) {
            s_discovery_last_ms = now;
            send_discovery_announcement();
        }
    }
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

void requestMdnsAdvertise() {
    s_mdns_refresh_due = true;
}

void saveWifi(const String &ssid, const String &pass) {
    wifi_store::put(ssid.c_str(), pass.c_str());
    logf("[wifi] saved ssid='%s' (pass len %u, total %u nets) - rebooting", ssid.c_str(),
         (unsigned)pass.length(), (unsigned)wifi_store::count());
    delay(200);
    ESP.restart();
}

static void append_log_locked(const char *line, int n) {
    if (!line || n <= 0) return;
    size_t idx = s_log_seq % LOG_RING_CAP;
    LogEntry &entry = s_log_ring[idx];
    entry.seq = ++s_log_seq;
    entry.ms = millis();
    size_t copy = n < (int)sizeof(entry.line) ? (size_t)n : sizeof(entry.line) - 1;
    memcpy(entry.line, line, copy);
    entry.line[copy] = 0;
    char *nl = strchr(entry.line, '\n');
    if (nl) *nl = 0;
    if (s_log_count < LOG_RING_CAP) s_log_count++;
}

size_t copyLogs(LogEntry *out, size_t cap, uint32_t since_seq) {
    if (!out || cap == 0) return 0;
    bool locked = false;
    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        locked = true;
    }

    size_t n = 0;
    uint32_t first_seq = s_log_seq >= s_log_count ? s_log_seq - s_log_count + 1 : 1;
    for (uint32_t seq = first_seq; seq <= s_log_seq && n < cap; ++seq) {
        if (since_seq && seq <= since_seq) continue;
        const LogEntry &entry = s_log_ring[(seq - 1) % LOG_RING_CAP];
        if (entry.seq == seq) out[n++] = entry;
    }

    if (locked) xSemaphoreGive(s_log_mtx);
    return n;
}

void clearLogs() {
    bool locked = false;
    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        locked = true;
    }
    s_log_count = 0;
    s_log_seq = 0;
    if (locked) xSemaphoreGive(s_log_mtx);
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
    append_log_locked(buf, n);
    fwrite(buf, 1, n, stdout);
    if (buf[n - 1] != '\n') putchar('\n');

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
        while (*p == ' ')
            p++;
        if (*p == '"') {
            const char *start = p + 1;
            const char *end = strchr(start, '"');
            if (!end) {
                logf("usage: wifi \"<ssid>\" [pass]   (missing closing quote)");
                return true;
            }
            ssid = String(start).substring(0, end - start);
            p = end + 1;
            while (*p == ' ')
                p++;
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
        {
            storage::Namespace prefs("net", false);
            prefs.remove("ssid");
            prefs.remove("pass");
        }
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
    if (line == "wifi-reconnect") {
        // Recovery hatch for the lwIP/ARP wedge: the radio reports the
        // STA associated but the IP layer stops answering ARP after some
        // request volume. Kicking disconnect+begin cycles the supplicant
        // and clears the lwIP netif without the 6+ second cost of
        // ESP.restart().  Credentials and saved networks survive.
        wl_status_t before = WiFi.status();
        logf("[wifi] reconnect requested (was status=%d ip=%s)", (int)before,
             ipString().c_str());
        WiFi.disconnect(false /* wifioff */, false /* erase */);
        delay(150);
        WiFi.reconnect();
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
    if (line == "id auto") {
        String id = hardware_default_device_id();
        {
            storage::Namespace prefs("net", false);
            prefs.put_string("device_id", id.c_str());
        }
        logf("[net] device id -> '%s' (hardware default, rebooting)", id.c_str());
        delay(200);
        ESP.restart();
        return true;
    }
    if (line.startsWith("id ")) {
        String id = line.substring(3);
        id.trim();
        if (id.length() == 0 || id.length() > 31) {
            logf("[net] id: name must be 1..31 chars");
            return true;
        }
        {
            storage::Namespace prefs("net", false);
            prefs.put_string("device_id", id.c_str());
        }
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
