#pragma once

// Spec 21 C: thin C++ wrapper over the ESP-IDF NVS API. Replaces
// `Preferences` site-by-site. RAII - the destructor commits on RW
// handles and closes always.
//
// On-disk layout (namespace names, key names, value types) is
// identical to `Preferences` so existing flashed devices keep
// their saved state when a module migrates over. The two APIs
// can coexist during the transition.
//
// All NVS access goes through this header so a future move off
// `nvs_*` (or onto a different backing store on a non-ESP target)
// has exactly one call site to retouch.
//
// String API: std::string (no Arduino dependency). Call sites that
// have an Arduino `String` use `s.c_str()` on input or
// `String(storage.get_string("k").c_str())` on output.

#include <stdint.h>
#include <stddef.h>
#include <string>

#include <nvs.h>

namespace storage {

class Namespace {
 public:
    // Open the named NVS namespace. `readonly=true` opens for read
    // only; `false` opens read-write and writes get committed in
    // the destructor. Failure to open is non-fatal - subsequent
    // gets return defaults and puts are no-ops; ok() reports the
    // failure if the caller cares.
    Namespace(const char *name, bool readonly);
    ~Namespace();

    // Cannot copy (would clone the nvs_handle_t and leak the close).
    Namespace(const Namespace &) = delete;
    Namespace &operator=(const Namespace &) = delete;

    bool ok() const { return ok_; }

    // ---- key probe ----------------------------------------------------
    // True iff `key` is present in this namespace. Matches the Arduino
    // Preferences::isKey() contract; used by config_runtime to choose
    // between "load v2" vs "migrate from legacy" paths.
    bool is_key(const char *key);

    // ---- get helpers (return default on missing-or-error) -------------
    std::string get_string(const char *key, const char *default_ = "");
    uint8_t     get_u8 (const char *key, uint8_t  default_ = 0);
    int8_t      get_i8 (const char *key, int8_t   default_ = 0);
    uint16_t    get_u16(const char *key, uint16_t default_ = 0);
    uint32_t    get_u32(const char *key, uint32_t default_ = 0);
    bool        get_bool(const char *key, bool default_ = false);
    // float and double are stored as a 4- or 8-byte blob to match the
    // wire layout Arduino Preferences uses (Preferences::putFloat /
    // putDouble both delegate to nvs_set_blob).
    float       get_float (const char *key, float  default_ = 0.0f);
    double      get_double(const char *key, double default_ = 0.0);

    // ---- put helpers (no-op when opened read-only or open failed) -----
    bool put_string(const char *key, const char *value);
    bool put_u8 (const char *key, uint8_t  value);
    bool put_i8 (const char *key, int8_t   value);
    bool put_u16(const char *key, uint16_t value);
    bool put_u32(const char *key, uint32_t value);
    bool put_bool(const char *key, bool value);
    bool put_float (const char *key, float  value);
    bool put_double(const char *key, double value);

    // Remove the key. No-op when not found or opened read-only.
    bool remove(const char *key);

    // Erase every key in this namespace. Matches Arduino
    // Preferences::clear(). No-op when opened read-only or open failed.
    bool erase_all();

 private:
    nvs_handle_t handle_{};
    bool readonly_{};
    bool ok_{};
    bool dirty_{};   // tracks whether a commit is needed at close
};

}  // namespace storage
