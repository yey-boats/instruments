#pragma once
// Minimal host shim for <Arduino.h> so the firmware UI headers (net.h,
// signalk.h, ...) compile in the native resolution harness. Only the surface
// the included headers reference is provided. NOT a faithful Arduino impl.

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// pgmspace shims: defining ARDUINO makes header-only libs (ArduinoJson) take
// their Arduino flash-string path, which expects these. On host, flash == RAM.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
typedef char __FlashStringHelper;
#define F(s) (s)
static inline uint8_t pgm_read_byte(const void *p) {
    return *(const uint8_t *)p;
}
static inline uint16_t pgm_read_word(const void *p) {
    return *(const uint16_t *)p;
}
static inline uint32_t pgm_read_dword(const void *p) {
    return *(const uint32_t *)p;
}
static inline float pgm_read_float(const void *p) {
    return *(const float *)p;
}
static inline void *pgm_read_ptr(const void *p) {
    return *(void *const *)p;
}
#define strlen_P strlen
#define strcmp_P strcmp
#define strncmp_P strncmp
#define memcpy_P memcpy
#define pgm_read_byte_near pgm_read_byte
#define pgm_read_word_near pgm_read_word

// Arduino String backed by std::string. Just the members the headers use.
class String {
  public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(double v, int = 2) : s_(std::to_string(v)) {}

    const char *c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    int toInt() const { return atoi(s_.c_str()); }
    double toDouble() const { return atof(s_.c_str()); }

    String &operator+=(const String &o) {
        s_ += o.s_;
        return *this;
    }
    String &operator+=(const char *o) {
        s_ += (o ? o : "");
        return *this;
    }
    String &operator+=(char c) {
        s_ += c;
        return *this;
    }
    String operator+(const String &o) const { return String(s_ + o.s_); }
    String operator+(const char *o) const { return String(s_ + (o ? o : "")); }
    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const { return s_ == (o ? o : ""); }
    bool operator!=(const String &o) const { return s_ != o.s_; }
    char operator[](size_t i) const { return i < s_.size() ? s_[i] : 0; }

    bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
    void trim() {
        size_t a = s_.find_first_not_of(" \t\r\n");
        size_t b = s_.find_last_not_of(" \t\r\n");
        s_ = (a == std::string::npos) ? "" : s_.substr(a, b - a + 1);
    }
    int indexOf(char c) const {
        auto p = s_.find(c);
        return p == std::string::npos ? -1 : (int)p;
    }
    String substring(size_t a) const { return String(s_.substr(a)); }
    String substring(size_t a, size_t b) const { return String(s_.substr(a, b - a)); }

  private:
    std::string s_;
};

// Host clock for millis(): stays 0 unless a harness advances it (sim_screens
// drives it alongside lv_tick_inc() to feed timed touch taps, instruments#73).
inline unsigned long g_sim_millis = 0;
static inline unsigned long millis() {
    return g_sim_millis;
}
static inline unsigned long micros() {
    return 0;
}
static inline void delay(unsigned long) {
}
static inline void delayMicroseconds(unsigned int) {
}
