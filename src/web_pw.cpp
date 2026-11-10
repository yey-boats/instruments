// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "web_pw.h"

#include <stdio.h>
#include <string.h>

namespace web_pw {

namespace {

constexpr const char PREFIX[] = "pbkdf2-sha256$";
constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *s, size_t hex_len, uint8_t *out) {
    for (size_t i = 0; i < hex_len / 2; ++i) {
        int hi = hex_val(s[2 * i]), lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void hex_encode(const uint8_t *in, size_t len, char *out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = kHex[in[i] >> 4];
        out[2 * i + 1] = kHex[in[i] & 0xF];
    }
    out[2 * len] = 0;
}

struct Parsed {
    uint32_t iterations;
    uint8_t salt[SALT_LEN];
    uint8_t dk[DK_LEN];
};

bool parse(const char *s, Parsed &p) {
    if (!s || strncmp(s, PREFIX, PREFIX_LEN) != 0) return false;
    const char *c = s + PREFIX_LEN;
    uint32_t it = 0;
    size_t digits = 0;
    while (*c >= '0' && *c <= '9') {
        if (++digits > 6) return false;
        it = it * 10 + (uint32_t)(*c - '0');
        ++c;
    }
    if (digits == 0 || it == 0 || it > ITERATIONS_MAX || *c != '$') return false;
    ++c;
    if (strnlen(c, 2 * SALT_LEN + 1) < 2 * SALT_LEN || c[2 * SALT_LEN] != '$') return false;
    if (!hex_decode(c, 2 * SALT_LEN, p.salt)) return false;
    c += 2 * SALT_LEN + 1;
    if (strnlen(c, 2 * DK_LEN + 1) != 2 * DK_LEN) return false;
    if (!hex_decode(c, 2 * DK_LEN, p.dk)) return false;
    p.iterations = it;
    return true;
}

}  // namespace

bool ct_equal(const void *a, size_t a_len, const void *b, size_t b_len) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t diff = (uint8_t)(a_len != b_len);
    for (size_t i = 0; i < a_len; ++i) {
        uint8_t yb = i < b_len ? y[i] : 0;
        diff |= (uint8_t)(x[i] ^ yb);
    }
    return diff == 0;
}

bool make_record(const char *pw, const uint8_t salt[SALT_LEN], uint32_t iterations, Kdf kdf,
                 char *out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = 0;
    if (!pw || !pw[0] || !salt || !kdf || iterations == 0 || iterations > ITERATIONS_MAX)
        return false;
    uint8_t dk[DK_LEN];
    if (!kdf(pw, strlen(pw), salt, SALT_LEN, iterations, dk)) return false;
    char salt_hex[2 * SALT_LEN + 1], dk_hex[2 * DK_LEN + 1];
    hex_encode(salt, SALT_LEN, salt_hex);
    hex_encode(dk, DK_LEN, dk_hex);
    memset(dk, 0, sizeof(dk));
    int w = snprintf(out, n, "%s%u$%s$%s", PREFIX, (unsigned)iterations, salt_hex, dk_hex);
    if (w < 0 || (size_t)w >= n) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool is_record(const char *s) {
    Parsed p;
    return parse(s, p);
}

bool verify(const char *record, const char *pw, size_t pw_len, Kdf kdf) {
    Parsed p;
    if (!kdf || !pw || !parse(record, p)) return false;
    if (pw_len == 0) return false;
    uint8_t dk[DK_LEN];
    if (!kdf(pw, pw_len, p.salt, SALT_LEN, p.iterations, dk)) return false;
    bool ok = ct_equal(p.dk, DK_LEN, dk, DK_LEN);
    memset(dk, 0, sizeof(dk));
    return ok;
}

bool split_basic(const char *decoded, size_t len, const char **user, size_t *user_len,
                 const char **pass, size_t *pass_len) {
    if (!decoded) return false;
    const char *colon = (const char *)memchr(decoded, ':', len);
    if (!colon) return false;
    *user = decoded;
    *user_len = (size_t)(colon - decoded);
    *pass = colon + 1;
    *pass_len = len - *user_len - 1;
    return true;
}

Migration migration(const char *plaintext, const char *record) {
    if (!plaintext || !plaintext[0]) return Migration::None;
    return is_record(record) ? Migration::DropPlaintext : Migration::HashPlaintext;
}

}  // namespace web_pw
