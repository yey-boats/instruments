#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace device_discovery {

constexpr uint16_t DEVICE_ANNOUNCE_PORT = 34301;
constexpr const char *DEVICE_ANNOUNCE_PROTOCOL = "espdisp.device.announce.v1";
constexpr const char *MDNS_SERVICE = "espdisp";
constexpr const char *MDNS_PROTO = "1";

struct Info {
    const char *device_id = "";
    const char *board_id = "";
    const char *firmware_name = "";
    const char *firmware_version = "";
    const char *ip = "";
    uint16_t port = 80;
    uint16_t display_width = 0;
    uint16_t display_height = 0;
    bool web_auth_required = false;
};

void build_announcement(JsonDocument &doc, const Info &info);

}  // namespace device_discovery
