#include "net.h"

#include <Arduino.h>
#include "storage.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

#include "board.h"
#include "psram_json.h"
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
#include "build_config.h"
#include "ota_pass.h"

namespace net {

static IPAddress broadcastAddr;
static bool ota_started = false;
// True only while an espota upload is in flight (between OTA onStart and
// onEnd / onError). Other tasks back off heap-pressuring work to give
// the upload uninterrupted bandwidth; see net::otaInProgress() callers.
static volatile bool s_ota_in_progress = false;

// Runtime log filter knobs. Honored only when YEYBOATS_DEBUG_UDP_LOG is
// compiled in; in release builds the UDP path is gone entirely so these
// just hold values nobody reads. Defaults are conservative (WARN+, no
// tag filter) so a freshly-flashed debug build doesn't drown the lab
// LAN on first boot.
static volatile uint8_t s_udp_log_level = LOG_WARN;
static char s_udp_log_tag[16] = {0};
static void load_log_prefs();

// Crash forensics. RTC slow memory survives software reset (panic,
// ESP.restart, watchdog) but not power-cycle, which is exactly the
// signal we want: a power loss is self-evident, a soft reset isn't.
// We stamp these on every log line, then at boot we read whatever
// remained from the previous run and broadcast it - the lab logger
// captures it as a `[prevboot]` trail, so we can see what was going
// on right before the device fell over even though the in-memory
// log ring was wiped by the reset.
//
// RTC_NOINIT_ATTR variables hold garbage on power-on. s_rtc_magic
// gates that - any first-boot read sees a junk value and we skip
// the dump, then stamp the magic for subsequent reboots.
//
// Size: 12 bytes of scalars + 2 KiB ring = well under the S3's 8 KiB
// of RTC slow mem and the chip doesn't use it for anything else here
// (no deep sleep in this firmware).
static constexpr uint32_t CRASH_RTC_MAGIC = 0x45535044;  // "ESPD"
static constexpr size_t CRASH_RING_CAP = 2048;
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_last_uptime_ms;
RTC_NOINIT_ATTR static uint32_t s_rtc_min_free_heap;
RTC_NOINIT_ATTR static uint32_t s_rtc_ring_head;

// Re-entrancy guard for the boot-time dump path. While we walk the
// RTC ring and re-emit each entry as `[prevboot] ...`, those log lines
// themselves go through log_emit() which calls crash_ring_append().
// Without this guard the new boot's ring fills with `[prevboot] `
// prefixed copies of the previous boot's content, and on the NEXT
// reset we end up with `[prevboot] [prevboot] [prevboot] ...` nested
// recursively, destroying the actual log content. Setting this to
// true around the dump tells crash_ring_append to skip - the in-memory
// log ring still captures everything for /api/logs.
static volatile bool s_rtc_dumping = false;
RTC_NOINIT_ATTR static char s_rtc_ring[CRASH_RING_CAP];

static void crash_ring_append(const char *buf, int n) {
    if (n <= 0) return;
    // Skip while we're dumping the previous-boot ring - otherwise the
    // dump's own log lines feed back into the ring and recurse on next
    // boot. See s_rtc_dumping declaration above.
    if (s_rtc_dumping) return;
    // Treat s_rtc_ring as a circular byte stream; head is the write
    // position. We don't track a separate tail - reader walks the
    // whole buffer at boot. Lines are NUL-separated so a torn write
    // mid-line is still parseable from the next separator.
    uint32_t head = s_rtc_ring_head % CRASH_RING_CAP;
    for (int i = 0; i < n; ++i) {
        s_rtc_ring[head] = buf[i];
        head = (head + 1) % CRASH_RING_CAP;
    }
    if (head < CRASH_RING_CAP) s_rtc_ring[head] = '\0';
    s_rtc_ring_head = head;
}

static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_UNKNOWN:
        return "UNKNOWN";
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "?";
    }
}

#ifdef YEYBOATS_DEBUG_UDP_LOG
// Lab-only UDP log sink. Compiled out of release builds entirely.
// Wire format: one logf() line per UDP datagram, broadcast to
// `broadcastAddr:UDP_LOG_PORT`. The lab logger (tools/lab-logger/)
// binds INADDR_ANY:UDP_LOG_PORT and prefixes each datagram with the
// source IP + receive timestamp before persisting.
static WiFiUDP s_log_udp;
static bool s_log_udp_ready = false;

// Extract the bracketed tag at the start of a log line: "[sk] foo" -> "sk".
// Returns nullptr if the line doesn't start with [tag].
static bool line_tag_matches(const char *buf, int n, const char *want) {
    if (!want || !*want) return true;  // no filter -> everything passes
    if (n < 2 || buf[0] != '[') return false;
    size_t wl = strlen(want);
    if ((int)wl + 2 > n) return false;
    if (strncmp(buf + 1, want, wl) != 0) return false;
    return buf[1 + wl] == ']';
}

static void udp_log_emit(uint8_t level, const char *buf, int n) {
    // Severity gate: higher numeric level == more verbose. We only emit
    // lines at-or-below the configured threshold.
    if (level > s_udp_log_level) return;
    if (!line_tag_matches(buf, n, s_udp_log_tag)) return;
    // broadcastAddr is set to softAPIP|.255 in AP mode and to
    // localIP | ~mask when STA comes up. Zero means we have no usable
    // network - silently drop (the in-memory log ring still has the
    // line for later inspection via /api/logs).
    if (broadcastAddr == IPAddress(0, 0, 0, 0)) return;
    if (!s_log_udp_ready) {
        if (!s_log_udp.begin(0)) return;
        s_log_udp_ready = true;
    }
    if (!s_log_udp.beginPacket(broadcastAddr, UDP_LOG_PORT)) return;
    s_log_udp.write((const uint8_t *)buf, (size_t)n);
    if (n == 0 || buf[n - 1] != '\n') s_log_udp.write((uint8_t)'\n');
    s_log_udp.endPacket();
}
#endif

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
    snprintf(buf, sizeof(buf), "yey-d-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return String(buf);
}

static String raw_efuse_device_id() {
    uint64_t mac = ESP.getEfuseMac();
    if (mac == 0) return String(OTA_HOSTNAME);
    char buf[32];
    snprintf(buf, sizeof(buf), "yey-d-%012llx", (unsigned long long)(mac & 0xffffffffffffULL));
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

// Old hardware-default id form ("espdisp-" + 12 lowercase-hex MAC), retired by
// the YEY Boats rebrand. A device still carrying an auto-generated old-prefix id
// is migrated to the new "yey-d-" prefix on boot. Operator-set names don't match
// this exact shape (prefix + exactly 12 hex), so they are preserved.
static bool is_legacy_prefixed_auto_id(const String &id) {
    static constexpr size_t kPrefixLen = 8;  // strlen("espdisp-")
    if (!id.startsWith("espdisp-")) return false;
    if (id.length() != kPrefixLen + 12) return false;
    for (size_t i = kPrefixLen; i < id.length(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool is_legacy_default_device_id(const String &id, const String &raw_efuse_id) {
    return id.length() == 0 || id == OTA_HOSTNAME || id == "espdisp-device" ||
           id == "yey-d-device" || id == raw_efuse_id || is_legacy_prefixed_auto_id(id);
}

bool isLegacyDefaultDeviceId(const String &id) {
    // Public version without the raw_efuse_id arg - manager.cpp doesn't
    // know the e-fuse value, but anyone trying to rename us TO a bare
    // e-fuse string is doing the wrong thing anyway, so the relaxed
    // check (no e-fuse comparison) is sufficient for the config-apply
    // safeguard.
    return id.length() == 0 || id == OTA_HOSTNAME || id == "espdisp-device" ||
           id == "yey-d-device" || is_legacy_prefixed_auto_id(id);
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
        bool inline_ok = (line == "ip" ||
#if YEYBOATS_ENABLE_BENCH
                          line == "bench" ||
#endif
                          line == "screen" || line == "sk-status" || line == "sk-dump" ||
                          line == "temp" || line == "wifi-list" || line == "bright");
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

static TaskHandle_t s_ota_task = nullptr;

// Drains ArduinoOTA on its own core-0 task. Previously this lived in
// net::loop() which runs from the Arduino main task alongside LVGL
// + touch + UI refresh; during a flash the main loop's LVGL work
// could starve ArduinoOTA.handle() for hundreds of ms, so the device
// would receive ~10 KiB then drop the TCP connection ("Error Uploading"
// on the host side after the first ~10 dots of espota.py output).
// Pinning OTA to core 0 with vTaskDelay(1) gives a tight ~1 ms poll
// regardless of what the UI is doing.
static void ota_task(void *) {
    for (;;) {
        if (ota_started) ArduinoOTA.handle();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static const char *ota_error_name(ota_error_t e) {
    switch (e) {
    case OTA_AUTH_ERROR:
        return "AUTH";
    case OTA_BEGIN_ERROR:
        return "BEGIN";
    case OTA_CONNECT_ERROR:
        return "CONNECT";
    case OTA_RECEIVE_ERROR:
        return "RECEIVE";
    case OTA_END_ERROR:
        return "END";
    default:
        return "?";
    }
}

static void otaSetup() {
    ArduinoOTA.setHostname(s_device_id.c_str());
    // Runtime OTA password (NVS) wins over the compile-time default.
    storage::Namespace prefs("net", true);
    std::string ota_nvs = prefs.get_string("ota_pass", "");
    const char *ota_pw = ota_pass_effective(ota_nvs.c_str(), OTA_PASSWORD);
    if (strlen(ota_pw) > 0) ArduinoOTA.setPassword(ota_pw);
    // All OTA lifecycle events log at WARN so they pass the default
    // UDP filter and land in the lab logger - critical for diagnosing
    // failed flashes (the previous build only printf'd to UART, which
    // is invisible on the field device).
    ArduinoOTA.onStart([]() {
        s_ota_in_progress = true;
        logf_at(LOG_WARN, "[ota] start cmd=%s free_heap=%u",
                ArduinoOTA.getCommand() == U_FLASH ? "flash" : "fs",
                (unsigned)esp_get_free_heap_size());
    });
    ArduinoOTA.onEnd([]() {
        s_ota_in_progress = false;
        logf_at(LOG_WARN, "[ota] end free_heap=%u", (unsigned)esp_get_free_heap_size());
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        // 10% milestones at WARN so we know how far the upload got
        // even when it later fails. Smaller increments would flood
        // the UDP path with low value.
        static unsigned int last_decile = 0xFFu;
        unsigned int pct = (p * 100) / t;
        unsigned int decile = pct / 10;
        if (decile != last_decile) {
            logf_at(LOG_WARN, "[ota] %u%% (%u/%u) heap=%u", pct, p, t,
                    (unsigned)esp_get_free_heap_size());
            last_decile = decile;
        }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        s_ota_in_progress = false;
        logf_at(LOG_WARN, "[ota] error %d (%s) free_heap=%u largest=%u", (int)e, ota_error_name(e),
                (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    });
    ArduinoOTA.begin();
    ota_started = true;
    // Pin the OTA pump to core 0 (network core) at the same priority as
    // sk_task. 4 KiB stack is plenty - Update class uses its own buffers
    // and ArduinoOTA.handle() doesn't recurse.
    if (!s_ota_task) {
        xTaskCreatePinnedToCore(ota_task, "ota", 4096, nullptr, 2, &s_ota_task, 0);
    }
    logf_at(LOG_WARN, "[ota] ready at %s.local task_pinned=core0", s_device_id.c_str());
}

// Try one network with a 10s timeout. Returns true on association.
static bool try_join(const char *ssid, const char *pass) {
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(s_device_id.c_str());
    // WiFi power-save: WIFI_PS_NONE is NOT viable here. ESP-IDF aborts
    // with "Should enable WiFi modem sleep when both WiFi and Bluetooth
    // are enabled" the moment BLE is also up - and BLE is the lab's
    // primary diagnostic channel so we can't drop it. Stay on the
    // default WIFI_PS_MIN_MODEM.
    //
    // Pin TX power to the default 19.5 dBm. Some Arduino-ESP32 builds
    // drop the default to 11 dBm to reduce thermal load; we'd rather
    // pay the heat to keep the link to the AP solid. (setListenInterval
    // would also be nice here for explicit responsiveness, but the
    // Arduino-ESP32 version on this build doesn't expose it; lower
    // levels would need esp_wifi_set_config(WIFI_IF_STA, ...) directly.)
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
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
    bool ok = WiFi.softAP("yey-d-setup", nullptr, 1, 0, 4);
    delay(500);  // allow the DHCP server task to spin up
    if (!ok) {
        puts("[wifi] softAP FAILED");
    }
    broadcastAddr = WiFi.softAPIP();
    broadcastAddr[3] = 255;
    ap_mode = true;
    printf("[wifi] AP ip=%s  mac=%s  ssid='yey-d-setup' (open)\n",
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
    // Control-protocol advertisement (design §4.1): pv = protocol version,
    // role = display|controller|both. A controller browsing _yeyboats._tcp reads
    // these to decide it can attach + drive this device over IP. Every yeyboats
    // node is a display that also accepts remote control, so role = "both".
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "pv", "2.0");
    MDNS.addServiceTxt(device_discovery::MDNS_SERVICE, "tcp", "role", "both");
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
    JsonDocument doc(&yeyboats::psram_json);
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
#ifdef YEYBOATS_DEBUG_UDP_LOG
        // Replay any log lines that were written before WiFi came up
        // (most importantly the `[boot] reset_reason=...` line that
        // emits in net::setup() before broadcastAddr is valid). One-shot
        // - new lines from this point on broadcast normally via the
        // standard logf() path.
        bool ringLocked = false;
        if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            ringLocked = true;
        }
        uint32_t first_seq = s_log_seq >= s_log_count ? s_log_seq - s_log_count + 1 : 1;
        for (uint32_t seq = first_seq; seq <= s_log_seq; ++seq) {
            const LogEntry &entry = s_log_ring[(seq - 1) % LOG_RING_CAP];
            if (entry.seq != seq) continue;
            int len = (int)strnlen(entry.line, sizeof(entry.line));
            if (len <= 0) continue;
            uint8_t lvl = entry.level ? entry.level : (uint8_t)LOG_INFO;
            udp_log_emit(lvl, entry.line, len);
        }
        if (ringLocked) xSemaphoreGive(s_log_mtx);
#endif
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
    // Load UDP log filter prefs before anything starts emitting so the
    // first boot-time logf doesn't get broadcast at the wrong level.
    load_log_prefs();

    // Crash forensics: emit one WARN line for every reboot describing
    // why we reset, and if the RTC ring has data from a prior soft
    // reset, dump its tail prefixed with [prevboot] so the lab logger
    // captures what was happening right before the panic. POWERON
    // reboots have garbage in the RTC area (uninitialized), so we gate
    // the prevboot dump on the magic word.
    {
        esp_reset_reason_t reason = esp_reset_reason();
        bool have_prev = (s_rtc_magic == CRASH_RTC_MAGIC);
        uint32_t free_now = (uint32_t)esp_get_free_heap_size();
        if (have_prev) {
            logf_at(LOG_WARN,
                    "[boot] reset_reason=%s prev_uptime_ms=%u "
                    "prev_min_free_heap=%u free_heap=%u",
                    reset_reason_name(reason), (unsigned)s_rtc_last_uptime_ms,
                    (unsigned)s_rtc_min_free_heap, (unsigned)free_now);
            // Walk the RTC ring from oldest to newest. head points at
            // the next write slot; the byte before it is the most
            // recently written. Lines are '\n'-terminated.
            //
            // Set s_rtc_dumping so crash_ring_append() skips while
            // we're emitting. Otherwise each `[prevboot] %s` line
            // feeds back into the ring and on the next reset we get
            // `[prevboot] [prevboot] [prevboot] ...` nested recursively,
            // destroying the actual log content (bug observed 2026-06-03).
            s_rtc_dumping = true;
            uint32_t head = s_rtc_ring_head % CRASH_RING_CAP;
            char line[200];
            size_t lp = 0;
            // Start one past head so we walk the full buffer once.
            for (size_t i = 0; i < CRASH_RING_CAP; ++i) {
                size_t idx = (head + i) % CRASH_RING_CAP;
                char c = s_rtc_ring[idx];
                if (c == '\0') continue;  // skip uninitialized / torn bytes
                if (c == '\n' || lp == sizeof(line) - 1) {
                    line[lp] = 0;
                    if (lp > 0) logf_at(LOG_WARN, "[prevboot] %s", line);
                    lp = 0;
                    continue;
                }
                if ((unsigned char)c >= 0x20) line[lp++] = c;
            }
            if (lp > 0) {
                line[lp] = 0;
                logf_at(LOG_WARN, "[prevboot] %s", line);
            }
            s_rtc_dumping = false;
        } else {
            logf_at(LOG_WARN, "[boot] reset_reason=%s (cold start, no prev trail) free_heap=%u",
                    reset_reason_name(reason), (unsigned)free_now);
        }
        // Reset the RTC region for the new boot's trail.
        s_rtc_magic = CRASH_RTC_MAGIC;
        s_rtc_ring_head = 0;
        s_rtc_last_uptime_ms = 0;
        s_rtc_min_free_heap = free_now;
        memset(s_rtc_ring, 0, CRASH_RING_CAP);
    }
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
#if YEYBOATS_ENABLE_INPUT_TEST
    if (input_test::handleConsoleCommand(line)) return true;
#endif
    if (manager::handleSerialCommand(line)) return true;
    if (beeper::handleSerialCommand(line)) return true;
    if (autopilot::handleSerialCommand(line)) return true;
    if (layout::handleSerialCommand(line)) return true;
    if (s_extra && s_extra(line)) return true;
    return false;
}

void loop() {
    // ArduinoOTA.handle() lives on the dedicated `ota` task pinned to
    // core 0 - see ota_task() above. Calling it from here too would
    // be redundant and (more importantly) couples OTA receive timing
    // to the Arduino main loop's LVGL/touch workload, which was the
    // observed cause of "Error Uploading" after ~10 KiB.
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

bool otaInProgress() {
    return s_ota_in_progress;
}
String ipString() {
    return ap_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

int rssi() {
    return wifiUp() ? WiFi.RSSI() : 0;
}

String ssidString() {
    // In STA the joined network; in AP mode the soft-AP name. Empty when down.
    if (ap_mode) return String("yey-d-setup");
    return wifiUp() ? WiFi.SSID() : String();
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

void setOtaPassword(const char *pw) {
    if (!pw) pw = "";
    storage::Namespace prefs("net", false);
    prefs.put_string("ota_pass", pw);
    ArduinoOTA.setPassword(strlen(pw) ? pw : OTA_PASSWORD);
    logf("[ota] password applied from config (len %u)", (unsigned)strlen(pw));
}

static void append_log_locked(const char *line, int n, uint8_t level) {
    if (!line || n <= 0) return;
    size_t idx = s_log_seq % LOG_RING_CAP;
    LogEntry &entry = s_log_ring[idx];
    entry.seq = ++s_log_seq;
    entry.ms = millis();
    entry.level = level;
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

static void log_emit(uint8_t level, const char *fmt, va_list ap) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (n == 0) return;

    bool locked = false;
    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        locked = true;
    }
    append_log_locked(buf, n, level);
    fwrite(buf, 1, n, stdout);
    if (buf[n - 1] != '\n') putchar('\n');

    // Forensic crash trail: copy every line into the RTC-retained ring
    // and stamp the current uptime + heap low-water. A panic kills the
    // device before it can flush; on next boot we read whatever survived
    // the soft reset. Cheap (memcpy + two stores) so it stays on for
    // all builds, not just debug.
    crash_ring_append(buf, n);
    if (buf[n - 1] != '\n') crash_ring_append("\n", 1);
    s_rtc_last_uptime_ms = millis();
    uint32_t min_free = (uint32_t)esp_get_minimum_free_heap_size();
    if (s_rtc_magic != CRASH_RTC_MAGIC || min_free < s_rtc_min_free_heap ||
        s_rtc_min_free_heap == 0) {
        s_rtc_min_free_heap = min_free;
    }

    if (bleConnected && bleTxChar) {
        bleTxChar->setValue((uint8_t *)buf, n);
        bleTxChar->notify();
    }
#ifdef YEYBOATS_DEBUG_UDP_LOG
    udp_log_emit(level, buf, n);
#else
    (void)level;
#endif
    if (locked) xSemaphoreGive(s_log_mtx);
}

void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_INFO, fmt, ap);
    va_end(ap);
}

void logf_at(LogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit((uint8_t)level, fmt, ap);
    va_end(ap);
}

static LogLevel parse_log_level(const char *s) {
    if (!s) return LOG_WARN;
    if (!strcasecmp(s, "error") || !strcmp(s, "1")) return LOG_ERROR;
    if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning") || !strcmp(s, "2")) return LOG_WARN;
    if (!strcasecmp(s, "info") || !strcmp(s, "3")) return LOG_INFO;
    if (!strcasecmp(s, "debug") || !strcmp(s, "4")) return LOG_DEBUG;
    if (!strcasecmp(s, "trace") || !strcmp(s, "5")) return LOG_TRACE;
    return LOG_WARN;
}

static const char *log_level_name(uint8_t lvl) {
    switch (lvl) {
    case LOG_ERROR:
        return "error";
    case LOG_WARN:
        return "warn";
    case LOG_INFO:
        return "info";
    case LOG_DEBUG:
        return "debug";
    case LOG_TRACE:
        return "trace";
    default:
        return "?";
    }
}

void setLogLevel(LogLevel max_level) {
    s_udp_log_level = (uint8_t)max_level;
    storage::Namespace prefs("log", false);
    prefs.put_u32("level", (uint32_t)max_level);
}
LogLevel logLevel() {
    return (LogLevel)s_udp_log_level;
}

void setLogTagFilter(const char *tag) {
    if (!tag) tag = "";
    // Strip leading '[' and trailing ']' for ergonomics: `log-tag sk`
    // and `log-tag [sk]` both work.
    if (tag[0] == '[') tag++;
    size_t tl = strlen(tag);
    if (tl > 0 && tag[tl - 1] == ']') tl--;
    if (tl >= sizeof(s_udp_log_tag)) tl = sizeof(s_udp_log_tag) - 1;
    memcpy(s_udp_log_tag, tag, tl);
    s_udp_log_tag[tl] = 0;
    storage::Namespace prefs("log", false);
    prefs.put_string("tag", s_udp_log_tag);
}
const char *logTagFilter() {
    return s_udp_log_tag;
}

static void load_log_prefs() {
    storage::Namespace prefs("log", true);
    uint32_t lvl = prefs.get_u32("level", LOG_WARN);
    if (lvl >= LOG_ERROR && lvl <= LOG_TRACE) s_udp_log_level = (uint8_t)lvl;
    std::string tag = prefs.get_string("tag", "");
    size_t tl = tag.size();
    if (tl >= sizeof(s_udp_log_tag)) tl = sizeof(s_udp_log_tag) - 1;
    memcpy(s_udp_log_tag, tag.data(), tl);
    s_udp_log_tag[tl] = 0;
}

bool handleSerialCommand(const String &line) {
    // Log filter knobs. Both persist to NVS so the device boots back into
    // the operator-chosen state without needing the lab box to push it.
    if (line == "log-status") {
        logf("[log] level=%s tag=%s udp=%s", log_level_name(s_udp_log_level),
             s_udp_log_tag[0] ? s_udp_log_tag : "(none)",
#ifdef YEYBOATS_DEBUG_UDP_LOG
             "enabled"
#else
             "disabled-by-build"
#endif
        );
        return true;
    }
    if (line.startsWith("log-level")) {
        String rest = line.length() > 9 ? line.substring(9) : String("");
        rest.trim();
        if (rest.length() == 0) {
            logf("[log] level=%s (usage: log-level error|warn|info|debug|trace)",
                 log_level_name(s_udp_log_level));
            return true;
        }
        LogLevel lvl = parse_log_level(rest.c_str());
        setLogLevel(lvl);
        logf("[log] level=%s", log_level_name(lvl));
        return true;
    }
    if (line.startsWith("log-tag")) {
        String rest = line.length() > 7 ? line.substring(7) : String("");
        rest.trim();
        if (rest.length() == 0) {
            logf("[log] tag=%s (usage: log-tag <name>|clear)",
                 s_udp_log_tag[0] ? s_udp_log_tag : "(none)");
            return true;
        }
        if (rest == "clear" || rest == "none" || rest == "off") rest = "";
        setLogTagFilter(rest.c_str());
        logf("[log] tag=%s", s_udp_log_tag[0] ? s_udp_log_tag : "(none)");
        return true;
    }
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
    if (line.startsWith("ota-pass ")) {
        String pw = line.substring(9);
        pw.trim();
        setOtaPassword(pw.c_str());
        printf("[ota] password set (len %u); effective next OTA\n", (unsigned)pw.length());
        return true;
    }
    if (line == "ota-pass-clear") {
        storage::Namespace prefs("net", false);
        prefs.put_string("ota_pass", "");
        printf("[ota] password cleared; reverts to compile-time default\n");
        return true;
    }
    if (line == "ota-pass") {
        storage::Namespace prefs("net", true);
        std::string cur = prefs.get_string("ota_pass", "");
        bool set = (cur.c_str()[0] != 0) || strlen(OTA_PASSWORD) > 0;
        printf("[ota] password %s (runtime %s)\n", set ? "set" : "unset",
               cur.c_str()[0] ? "yes" : "no");
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
        logf("[wifi] reconnect requested (was status=%d ip=%s)", (int)before, ipString().c_str());
        WiFi.disconnect(false /* wifioff */, false /* erase */);
        delay(150);
        WiFi.reconnect();
        return true;
    }
    if (line == "ip") {
        logf("ip=%s  mode=%s  rssi=%d", ipString().c_str(), ap_mode ? "AP" : "STA", WiFi.RSSI());
        return true;
    }
    if (line == "temp") {
        float tC = board::chipTempC();
        if (isnan(tC))
            logf("[temp] chip temp unavailable");
        else
            logf("[temp] chip=%.1f C", tC);
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
