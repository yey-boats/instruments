#pragma once

// Stable device identity for the management plane (docs/specs/17 F1).
//
// Surfaces the fields the manager plugin (spec 18) wants for the
// device registry: stable id, hardware fingerprint, firmware build
// metadata, and capability flags. The string fields are owned by
// this module (file-static buffers) so callers can pass them around
// as `const char *` without copy churn.
//
// Build metadata (firmware version, git, build time) come from
// preprocessor defines injected by platformio (see build_flags in
// platformio.ini); they fall back to placeholders if absent.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>

namespace device_identity {

struct Capabilities {
    bool touch;
    bool touch_irq;
    bool ble_config;
    bool arduino_ota;
    bool pull_ota;  // false until spec 17 F6 lands
    bool nmea0183_wifi;
    bool nmea2000;  // true only if -DENABLE_NMEA2000 + transceiver
    bool autopilot_controls;
    bool beeper;
    bool local_web_ui;
};

struct Identity {
    const char *device_id;         // user-set, via `id <name>` CLI
    const char *mac;               // 12-char lowercase hex
    const char *board_id;          // e.g. "sunton_4848s040"
    const char *chip;              // e.g. "esp32-s3"
    const char *firmware_name;     // "espdisp"
    const char *firmware_version;  // semver or 0.0.0+<short-sha>
    const char *build_time;        // __DATE__ " " __TIME__
    const char *git_commit;        // short SHA or "unknown"
    const char *pio_env;           // "esp32-4848s040"
    uint32_t flash_total_kb;
    uint32_t psram_total_kb;
};

// Initialise once. Safe to call repeatedly.
void setup();

// Stable snapshot. Pointers remain valid for the lifetime of the
// firmware.
const Identity &get();
const Capabilities &capabilities();

// Serialise identity + capabilities into a JsonDocument the caller
// owns. Used by the manager registration payload.
void to_json_doc(JsonDocument &doc);

}  // namespace device_identity
