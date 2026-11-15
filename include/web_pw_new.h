#pragma once

// A web password made ON a standalone display (no Display Manager).
//
// Since instruments#30 the display keeps only a salted PBKDF2 record of its
// web password (web_pw.h); the plaintext normally comes from the Display
// Manager's webAuth push and stays with the manager. A display that was never
// paired has no password and its web UI + /editor are open ("setup-open").
// For that case the Settings screen offers "new pass" and the console has
// `web-pass-new`: the device generates a random password, stores only its
// record (web_pw_store::set_password, the same path the manager push uses),
// turns Basic auth on, and shows the plaintext once (screen for REVEAL_MS, or
// one console line). If a manager pairs later its pushed password replaces
// this one as before.
//
// Not offered on a managed display: the manager keeps the plaintext and
// would otherwise hold a stale copy.
//
// Pure C++ (host-tested in test/test_web_pw_new): alphabet + generator, the
// offer/gating rule, the press-twice confirm window and the timed reveal.

#include <stddef.h>
#include <stdint.h>

#include "web_auth_policy.h"

namespace web_pw_new {

// Unambiguous on a small display font and when typed: no 0/O/o, 1/l/I.
constexpr const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
constexpr size_t ALPHABET_LEN = sizeof(ALPHABET) - 1;  // 56 -> ~5.8 bits/char
constexpr size_t LENGTH = 12;                          // ~69 bits
constexpr size_t BUF_LEN = LENGTH + 1;

// Second press must land within this window or the button disarms.
constexpr uint32_t CONFIRM_MS = 5000;
// How long the new plaintext stays on screen before it is wiped.
constexpr uint32_t REVEAL_MS = 30000;

// The console command (serial / BLE NUS only, see is_console_only).
constexpr const char *COMMAND = "web-pass-new";

// Uniform 32-bit random source (esp_fill_random on the device).
using Rng = uint32_t (*)();

// LENGTH chars from ALPHABET, rejection-sampled (no modulo bias), NUL
// terminated. False (out = "" when n > 0) when out is too small or rng null.
bool generate(Rng rng, char *out, size_t n);

// True if `pw` is exactly LENGTH chars, all from ALPHABET.
bool well_formed(const char *pw);

// Overwrite a secret buffer in a way the compiler may not elide.
void wipe(void *p, size_t n);

// Whether the action is offered, and what it changes.
struct Offer {
    bool available;      // false on a managed display (manager owns the password)
    bool turns_auth_on;  // currently setup-open or owner opt-out: login becomes required
};
Offer offer(bool managed, web_auth_policy::State state);

// Commands that must not run from /api/cmd or a MIDL button: the result is
// printed to the local console only, so a LAN client (or a record's button)
// could change the login without anyone seeing the new password.
bool is_console_only(const char *line);

// Press-twice confirm: the first press arms, a second press within
// CONFIRM_MS fires; a later press re-arms.
class Confirm {
  public:
    enum class Press : uint8_t { Armed, Fire };
    Press press(uint32_t now_ms);
    // Armed and not yet expired (the button shows "confirm?").
    bool armed(uint32_t now_ms) const;
    void reset() { armed_ = false; }

  private:
    bool armed_ = false;
    uint32_t at_ms_ = 0;
};

// The on-screen plaintext and its expiry. The buffer is wiped on hide().
class Reveal {
  public:
    void show(const char *pw, uint32_t now_ms);
    // Visible and inside REVEAL_MS.
    bool visible(uint32_t now_ms) const;
    // Whole seconds left (rounded up), 0 when not visible.
    uint32_t seconds_left(uint32_t now_ms) const;
    // Wipes once the window has passed; true if it just hid.
    bool expire(uint32_t now_ms);
    void hide();
    const char *password() const { return pw_; }

  private:
    char pw_[BUF_LEN] = {0};
    uint32_t at_ms_ = 0;
    bool on_ = false;
};

}  // namespace web_pw_new
