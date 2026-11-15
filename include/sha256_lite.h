#pragma once

// Small, dependency-free SHA-256 (FIPS 180-4). Pure C++ so the MIDL record
// store hashes the same bytes the same way on the device and in the native
// tests (mbedtls is device-only). Not constant-time; used for content hashes
// only, never for secrets.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>
#include <stdint.h>

namespace sha256_lite {

struct Ctx {
    uint32_t h[8];
    uint64_t len;  // total bytes fed
    uint8_t buf[64];
    size_t buf_len;
};

void init(Ctx &c);
void update(Ctx &c, const void *data, size_t len);
void final(Ctx &c, uint8_t out[32]);

// One-shot: writes "sha256:<64 lowercase hex>" + NUL into out (cap >= 72).
// Returns false (out = "") when cap is too small.
bool hash_tagged(const void *data, size_t len, char *out, size_t cap);

}  // namespace sha256_lite
