#pragma once

// NVS-backed list of saved WiFi networks. Boot iterates them in order
// and joins the first that associates.

#include <Arduino.h>
#include <stddef.h>

namespace wifi_store {

constexpr size_t MAX_NETWORKS = 8;
constexpr size_t SSID_MAX = 32;
constexpr size_t PASS_MAX = 64;

struct Network {
    char ssid[SSID_MAX + 1];
    char pass[PASS_MAX + 1];
    bool used;
};

// Load networks from NVS. Call once at boot.
void load();

// Read-only access. Iterate from 0 to count() - 1.
size_t count();
const Network &at(size_t index);

// Add or update a network (matched by ssid). Moves it to the front so
// the most-recently-saved wins on next boot. Persists to NVS. Returns
// the new count.
size_t put(const char *ssid, const char *pass);

// Remove by ssid. Returns true if removed.
bool remove(const char *ssid);

// Wipe all entries.
void clear_all();

// Migrate the legacy single-pair NVS key ("ssid"/"pass" in "net"
// namespace) into the list on first boot. Idempotent.
void migrate_legacy_if_any();

// Serialize to a JSON array string (without passwords by default, or with
// when `with_passwords` is true - the latter is for BLE/console only).
String to_json(bool with_passwords = false);

}  // namespace wifi_store
