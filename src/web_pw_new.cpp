// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "web_pw_new.h"

#include <string.h>

namespace web_pw_new {

static_assert(ALPHABET_LEN == 56, "alphabet changed: update the entropy note");

bool generate(Rng rng, char *out, size_t n) {
    if (out && n) out[0] = '\0';
    if (!rng || !out || n < BUF_LEN) return false;
    // Largest multiple of ALPHABET_LEN that fits in 32 bits: values at or
    // above it are redrawn so every char is equally likely.
    const uint32_t limit = (uint32_t)(0x100000000ULL / ALPHABET_LEN * ALPHABET_LEN);
    for (size_t i = 0; i < LENGTH; ++i) {
        uint32_t r;
        do {
            r = rng();
        } while (r >= limit);
        out[i] = ALPHABET[r % ALPHABET_LEN];
    }
    out[LENGTH] = '\0';
    return true;
}

bool well_formed(const char *pw) {
    if (!pw || strlen(pw) != LENGTH) return false;
    for (size_t i = 0; i < LENGTH; ++i)
        if (!memchr(ALPHABET, pw[i], ALPHABET_LEN)) return false;
    return true;
}

void wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (p && n--)
        *v++ = 0;
}

Offer offer(bool managed, web_auth_policy::State state) {
    Offer o{};
    if (managed) return o;
    o.available = true;
    o.turns_auth_on = state != web_auth_policy::State::Enforced;
    return o;
}

bool is_console_only(const char *line) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t')
        ++line;
    size_t n = strlen(COMMAND);
    if (strncmp(line, COMMAND, n) != 0) return false;
    char c = line[n];
    return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Confirm::Press Confirm::press(uint32_t now_ms) {
    if (armed(now_ms)) {
        armed_ = false;
        return Press::Fire;
    }
    armed_ = true;
    at_ms_ = now_ms;
    return Press::Armed;
}

bool Confirm::armed(uint32_t now_ms) const {
    return armed_ && (uint32_t)(now_ms - at_ms_) < CONFIRM_MS;
}

void Reveal::show(const char *pw, uint32_t now_ms) {
    wipe(pw_, sizeof(pw_));
    if (!pw) return;
    strncpy(pw_, pw, sizeof(pw_) - 1);
    pw_[sizeof(pw_) - 1] = '\0';
    at_ms_ = now_ms;
    on_ = true;
}

bool Reveal::visible(uint32_t now_ms) const {
    return on_ && (uint32_t)(now_ms - at_ms_) < REVEAL_MS;
}

uint32_t Reveal::seconds_left(uint32_t now_ms) const {
    if (!visible(now_ms)) return 0;
    uint32_t left = REVEAL_MS - (uint32_t)(now_ms - at_ms_);
    return (left + 999) / 1000;
}

bool Reveal::expire(uint32_t now_ms) {
    if (!on_ || visible(now_ms)) return false;
    hide();
    return true;
}

void Reveal::hide() {
    wipe(pw_, sizeof(pw_));
    on_ = false;
    at_ms_ = 0;
}

}  // namespace web_pw_new
