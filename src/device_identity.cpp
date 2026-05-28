#include "device_identity.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "net.h"

#ifndef FW_NAME
#define FW_NAME "espdisp"
#endif
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif
#ifndef FW_GIT_COMMIT
#define FW_GIT_COMMIT "unknown"
#endif
#ifndef PIO_ENV
#define PIO_ENV "esp32-4848s040"
#endif

namespace device_identity {

namespace {

char s_mac[13] = {0};
char s_chip[16] = {0};
char s_build_time[32] = {0};
Identity s_identity = {};
Capabilities s_caps = {};
bool s_init = false;

void fill_mac() {
    uint8_t m[6] = {0};
    WiFi.macAddress(m);
    snprintf(s_mac, sizeof(s_mac), "%02x%02x%02x%02x%02x%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

void fill_chip() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char *model = "esp32";
    switch (info.model) {
        case CHIP_ESP32:    model = "esp32"; break;
        case CHIP_ESP32S2:  model = "esp32-s2"; break;
        case CHIP_ESP32S3:  model = "esp32-s3"; break;
        case CHIP_ESP32C3:  model = "esp32-c3"; break;
        default:            model = "esp32"; break;
    }
    snprintf(s_chip, sizeof(s_chip), "%s-r%u", model, info.revision);
}

}  // namespace

void setup() {
    if (s_init) return;
    fill_mac();
    fill_chip();
    snprintf(s_build_time, sizeof(s_build_time), "%s %s", __DATE__, __TIME__);

    s_identity.device_id = net::deviceId().c_str();
    s_identity.mac = s_mac;
    s_identity.board_id = board::id();
    s_identity.chip = s_chip;
    s_identity.firmware_name = FW_NAME;
    s_identity.firmware_version = FW_VERSION;
    s_identity.build_time = s_build_time;
    s_identity.git_commit = FW_GIT_COMMIT;
    s_identity.pio_env = PIO_ENV;

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    s_identity.flash_total_kb = flash_size / 1024;
    s_identity.psram_total_kb =
        (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);

    board::Capabilities bc = board::capabilities();
    s_caps.touch = bc.touch != board::TouchKind::None;
    s_caps.touch_irq = false;  // 4848s040 has no INT routed
    s_caps.ble_config = true;
    s_caps.arduino_ota = true;
    s_caps.pull_ota = false;   // F6 future
    s_caps.nmea0183_wifi = true;
#ifdef ENABLE_NMEA2000
    s_caps.nmea2000 = bc.nmea2000_can;
#else
    s_caps.nmea2000 = false;
#endif
    s_caps.autopilot_controls = true;
    s_caps.beeper = bc.beeper;
    s_caps.local_web_ui = true;

    s_init = true;
}

const Identity &get() {
    if (!s_init) setup();
    // device_id can be re-read each call since net::deviceId() may
    // mutate via the `id <name>` CLI; refresh the pointer.
    s_identity.device_id = net::deviceId().c_str();
    return s_identity;
}

const Capabilities &capabilities() {
    if (!s_init) setup();
    return s_caps;
}

void to_json_doc(JsonDocument &doc) {
    const Identity &i = get();
    const Capabilities &c = capabilities();
    doc["deviceId"] = i.device_id;
    doc["mac"] = i.mac;
    doc["board_id"] = i.board_id;
    doc["chip"] = i.chip;
    doc["firmware_name"] = i.firmware_name;
    doc["firmware_version"] = i.firmware_version;
    doc["build_time"] = i.build_time;
    doc["git_commit"] = i.git_commit;
    doc["pio_env"] = i.pio_env;
    doc["flash_total_kb"] = i.flash_total_kb;
    doc["psram_total_kb"] = i.psram_total_kb;
    JsonObject caps = doc["capabilities"].to<JsonObject>();
    caps["touch"] = c.touch;
    caps["touch_irq"] = c.touch_irq;
    caps["ble_config"] = c.ble_config;
    caps["arduino_ota"] = c.arduino_ota;
    caps["pull_ota"] = c.pull_ota;
    caps["nmea0183_wifi"] = c.nmea0183_wifi;
    caps["nmea2000"] = c.nmea2000;
    caps["autopilot_controls"] = c.autopilot_controls;
    caps["beeper"] = c.beeper;
    caps["local_web_ui"] = c.local_web_ui;
}

}  // namespace device_identity
