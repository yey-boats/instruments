#pragma once

// Pure-C++ hostname validator shared by manager apply_config and any
// future BLE / web hostname-set path.
//
// Rules (intentionally a strict subset of RFC 952 / 1123 / 1035 host
// label rules that the GT911 board's mDNS + WiFi hostname slots all
// accept):
//
//   - non-empty
//   - 1..31 chars (matches the device_id slot in NVS)
//   - each char is [A-Z], [a-z], [0-9], or '-'
//   - first char must NOT be '-'
//   - last char must NOT be '-'
//
// Returns true iff the candidate would be accepted by the apply path.
// Use this from the manager validator AND any new entry point so the
// same rules can't drift across surfaces.

#include <stddef.h>

namespace hostname_check {

constexpr size_t MAX_LEN = 31;

bool is_valid(const char *s);

}  // namespace hostname_check
