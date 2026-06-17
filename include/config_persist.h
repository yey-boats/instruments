#pragma once

// Slice 4 — flash-persistent applied config: the pure decision logic for
// "should we persist / re-apply / re-fetch" given the persisted-vs-desired
// config hashes. Extracted from manager.cpp so it is host-testable and the
// same rules can't drift across the apply, boot-load, and drift-check
// surfaces.
//
// Pure C++ (std::string only, no Arduino / NVS / flash). The flash I/O
// itself lives in manager.cpp (device-only); this module decides *whether*
// to touch flash, never *how*.
//
// Project: esp32-boat-mfd. (c) navado and contributors.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstdint>
#include <string>

namespace config_persist {

// Schema/manifest version stamped alongside a persisted blob so a firmware
// that changed its config schema can reject an incompatible persisted blob
// on boot instead of mis-parsing it. Bump when the on-flash config shape
// changes in a way an older parser couldn't honor.
constexpr uint8_t SCHEMA_VERSION = 1;

// ---- write-on-apply -----------------------------------------------------
// Decide whether a freshly applied config should be written to flash.
//
//   applied_ok    : apply_config() returned true (every field honored).
//                   On false the caller applied a partial subset and MUST
//                   NOT persist (matches the existing "not persisted" log).
//   new_hash      : the config hash the manager advertised for this blob.
//   persisted_hash: the hash currently stored in flash ("" if none).
//
// Persist iff the apply succeeded, the new hash is non-empty, and it
// differs from what is already on flash — so an unchanged re-apply does
// not burn a flash erase/write cycle (the spec's flash-wear rule).
bool should_persist(bool applied_ok, const std::string &new_hash,
                    const std::string &persisted_hash);

// ---- load-on-boot -------------------------------------------------------
// Decide whether a persisted blob found on flash should be applied at boot.
//
//   has_blob       : a non-empty config blob was read from flash.
//   blob_schema    : the schema version stamped with that blob.
//   running_schema : SCHEMA_VERSION of the firmware now booting.
//
// Apply iff a blob exists and its schema matches the running firmware. A
// schema mismatch means the blob predates a schema change; ignore it (the
// caller should clear it) and fall back to built-in defaults rather than
// risk mis-parsing.
bool should_apply_on_boot(bool has_blob, uint8_t blob_schema, uint8_t running_schema);

// ---- drift check on reconnect -------------------------------------------
// Decide whether the device should fetch a new config from the manager.
//
//   desired_hash : the hash the manager advertises it wants (heartbeat).
//   applied_hash : the hash the device currently has applied. After a
//                  boot-from-flash this is the persisted hash, so an
//                  unchanged config does NOT trigger a re-fetch.
//   desired_ver / applied_ver : same comparison on the version label, for
//                  managers that drive drift by version rather than hash.
//
// Fetch iff the manager advertised a hash/version that is non-empty AND
// differs from what the device has applied. This is the existing drift
// rule; pinned here so the persisted-hash-after-boot path keeps it honest.
bool should_refetch(const std::string &desired_hash, const std::string &applied_hash,
                    const std::string &desired_ver, const std::string &applied_ver);

}  // namespace config_persist
