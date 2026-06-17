// Slice 4 — pure decision logic for flash-persistent applied config.
// See include/config_persist.h. Pure C++; host-tested in
// test/test_config_persist. No Arduino / NVS / flash here.
//
// Project: esp32-boat-mfd. (c) navado and contributors.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "config_persist.h"

namespace config_persist {

bool should_persist(bool applied_ok, const std::string &new_hash,
                    const std::string &persisted_hash) {
    if (!applied_ok) return false;
    if (new_hash.empty()) return false;
    return new_hash != persisted_hash;
}

bool should_apply_on_boot(bool has_blob, uint8_t blob_schema, uint8_t running_schema) {
    if (!has_blob) return false;
    return blob_schema == running_schema;
}

bool should_refetch(const std::string &desired_hash, const std::string &applied_hash,
                    const std::string &desired_ver, const std::string &applied_ver) {
    if (!desired_hash.empty() && desired_hash != applied_hash) return true;
    if (!desired_ver.empty() && desired_ver != applied_ver) return true;
    return false;
}

}  // namespace config_persist
