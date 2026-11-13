#pragma once
// instruments#30: ArduinoOTA (espota, TCP/UDP 3232) policy.
//
// ArduinoOTA accepts a full firmware image from any LAN peer that knows the
// password, and with no password from ANY LAN peer, which defeats the web
// API's Basic Auth entirely. So:
//
//   - Release builds (YEYBOATS_RELEASE_BUILD) compile ArduinoOTA out. Field
//     updates go through the manager's pull OTA (manager.cpp: the device
//     polls /devices/<id>/firmware/jobs, downloads the image over HTTP with
//     its device token, verifies sha256, then Update.*), or USB.
//   - Dev builds run ArduinoOTA only with a password: the runtime one in NVS
//     net/ota_pass (`ota-pass <pw>` on serial/BLE, or the manager's
//     ota.password) or the compile-time OTA_PASSWORD from secrets.h.
//   - A dev build with no password keeps ArduinoOTA OFF, unless it was built
//     with the lab-only -D YEYBOATS_LAB_OPEN_OTA=1 (e.g. `make ota
//     LAB_OPEN_OTA=1`). No env in platformio.ini sets it, and net.cpp #errors
//     if it meets YEYBOATS_RELEASE_BUILD (CI: tools/test_web_security.py).
//
// Pure (no Arduino) so the decision is host-tested in test/test_ota_pass.

// build_config.h always defines YEYBOATS_RELEASE_BUILD (0 for dev builds), so
// it is tested by VALUE: `defined()` would be true for every build that
// included build_config.h first.
#include "build_config.h"

#if YEYBOATS_RELEASE_BUILD
#define YEYBOATS_ARDUINO_OTA 0
#else
#define YEYBOATS_ARDUINO_OTA 1
#endif

namespace ota_policy {

enum class Mode {
    Disabled,  // ArduinoOTA not started (release build, or dev with no password)
    Password,  // ArduinoOTA with the effective password
    LabOpen,   // dev + YEYBOATS_LAB_OPEN_OTA + no password: open (lab only)
};

inline Mode mode(bool release_build, bool lab_open_flag, const char *effective_password) {
    if (release_build) return Mode::Disabled;
    if (effective_password && effective_password[0]) return Mode::Password;
    return lab_open_flag ? Mode::LabOpen : Mode::Disabled;
}

inline const char *mode_name(Mode m) {
    switch (m) {
    case Mode::Password:
        return "password";
    case Mode::LabOpen:
        return "lab-open";
    case Mode::Disabled:
    default:
        return "disabled";
    }
}

// The compile-time inputs for mode(), for the current build.
inline constexpr bool kReleaseBuild =
#if YEYBOATS_RELEASE_BUILD
    true;
#else
    false;
#endif
inline constexpr bool kLabOpenFlag =
#if defined(YEYBOATS_LAB_OPEN_OTA)
    true;
#else
    false;
#endif

}  // namespace ota_policy
