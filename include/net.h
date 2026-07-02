#pragma once

#include <Arduino.h>

namespace net {

void setup();  // call once after Serial.begin
void loop();   // call frequently from main loop

// Log severity. The default `logf()` emits at INFO. UDP broadcast (debug
// builds only) is gated by a runtime threshold + optional tag filter, so
// chatty INFO/DEBUG/TRACE traffic can't flood the lab LAN unless an
// operator explicitly turns it on via the `log-level` / `log-tag`
// commands. stdout, BLE NUS, and the in-memory ring buffer are
// unconditional - they're the low-cost local sinks.
enum LogLevel : uint8_t {
    LOG_ERROR = 1,
    LOG_WARN = 2,
    LOG_INFO = 3,
    LOG_DEBUG = 4,
    LOG_TRACE = 5,
};

// Multi-target log: Serial + UDP broadcast + BLE notify (if connected).
// Safe to call before setup() - it falls through to Serial only.
// `logf` defaults to INFO; use `logf_at(level, ...)` to classify a line
// explicitly. Lines that should reach the lab log collector in normal
// operation must be WARN or ERROR (the default UDP threshold).
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf_at(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Runtime knobs for the UDP log filter (debug builds only; no-op in
// release). Both are NVS-persisted under namespace "log". Pass an empty
// string to logTagFilter() to clear the tag filter (all tags pass).
void setLogLevel(LogLevel max_level);
LogLevel logLevel();
void setLogTagFilter(const char *tag);
const char *logTagFilter();

struct LogEntry {
    uint32_t seq;
    uint32_t ms;
    uint8_t level;  // see LogLevel; 0 == legacy/INFO
    char line[192];
};

// Copy the rotating in-memory device log. Returns number of entries copied
// in chronological order. `since_seq` skips older entries when non-zero.
size_t copyLogs(LogEntry *out, size_t cap, uint32_t since_seq = 0);
void clearLogs();

// Handle a line of input on Serial console (e.g. "wifi <ssid> <pass>").
// Returns true if the line was consumed by net.
bool handleSerialCommand(const String &line);

bool wifiUp();
String ipString();
String ssidString();
int rssi();

// The setup AP's current WPA2 password (NVS override via `ap-pass <pass>`,
// else the MAC-suffixed default `yeyboats-<MAC4>` that start_ap_mode()
// computes). Empty string means the AP is open (`ap-pass open`). Only
// meaningful once the device has entered AP mode (compute_ap_credentials()
// has run); used by the WiFi setup screen to render the join QR/label.
String apPassword();

// True for "placeholder" device IDs the firmware has used historically:
// empty string, OTA_HOSTNAME (the secrets.h default), the literal
// "espdisp-device" (the original legacy fallback), or the bare e-fuse
// MAC form. The MAC-derived `espdisp-<mac>` IDs are NOT placeholders.
// Used by the manager config-apply path to refuse a rename TO a
// placeholder name, which previously caused a reboot loop when the
// plugin's default config carried network.hostname=espdisp-device.
bool isLegacyDefaultDeviceId(const String &id);

// WiFi manager state. Set by the async wifi_manager_task; readable from
// any task.
enum class WifiState : uint8_t {
    Idle,        // setup() not yet called
    Connecting,  // trying saved networks
    StaUp,       // STA joined, mDNS / OTA up
    ApSetup,     // STA failed (or no creds) - AP fallback active
    Failed,      // unrecoverable
};
WifiState wifiState();
const char *wifiStateName();

// Persist WiFi credentials (NVS) and reboot. Pass empty `pass` for open
// networks. SSIDs with spaces are fine. Prefer joinWifi() (no reboot);
// this remains as the `wifi-reboot` escape hatch and for callers that
// depend on the reboot (harness first-boot seeding).
void saveWifi(const String &ssid, const String &pass);

// Persist WiFi credentials (NVS) and join live WITHOUT a reboot (NET-2).
// Spawns a one-shot join task (association blocks up to 10 s per network),
// so it is safe to call from any task including the LVGL task. On success
// the STA comes up in place (mDNS/OTA/web keep running); on failure it
// falls back to the other saved networks, then the setup AP.
void joinWifi(const String &ssid, const String &pass);

// Persist a manager-pushed OTA password to NVS (same key as the `ota-pass`
// console command) and apply it immediately. Empty string clears to the
// compile-time default. Does NOT log the password value.
void setOtaPassword(const char *pw);

// User-configurable device identity (BLE name, mDNS host, OTA host).
// Default = espdisp-<wifi-sta-mac>; legacy OTA_HOSTNAME values migrate to it.
const String &deviceId();

// Ask the Bonjour/mDNS responder to refresh the espdisp service TXT records.
// Used after config/auth changes; boot and periodic refreshes are handled by
// net::loop().
void requestMdnsAdvertise();

// Dispatch a command line through net / sk / layout / main handlers in
// order. Returns true if any handler consumed it. BLE / serial / extra
// callers all funnel through this.
bool dispatchCommand(const String &line);

// Optional extra command handler registered by main; called for lines net::
// and sk:: didn't consume. Returns true if the line was handled.
using ExtraCommandHandler = bool (*)(const String &line);
void setExtraCommandHandler(ExtraCommandHandler h);

// True while an inbound espota upload is in flight (between onStart and
// onEnd/onError). Heap-pressuring subsystems (manager heartbeat loop,
// SK delta JsonDocument churn, web heavy endpoints) should back off
// while this is set so the OTA stream gets uninterrupted WiFi TX
// bandwidth and the internal heap stays headroom-safe for the Update
// class's working buffers.
bool otaInProgress();

}  // namespace net
