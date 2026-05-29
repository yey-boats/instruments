#pragma once

// Pure parser for the log-level token accepted by:
//   - spec 17 §8 log.level v1 command (payload.level)
//   - spec 17 §6 cfg["debug"]["logLevel"]
//
// Accepts both the string form ("trace"|"verbose"|"debug"|"info"|
// "warn"|"error"|"none") and the numeric ESP_LOG_* enum value (0..5).
// The returned int is the matching esp_log_level_t value (so the
// firmware can cast it without depending on this header pulling esp
// headers in).
//
// Pure C++ - host-testable. The firmware caller decides what to do
// with the level (typically esp_log_level_set("*", out)).

namespace log_level_check {

// 0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE. Mirrors the
// esp_log_level_t enum but kept here so this header doesn't include
// esp_log.h on the host build.
constexpr int kMin = 0;
constexpr int kMax = 5;

// Parse a string form. Returns true on success and sets *out;
// returns false (out untouched) for null / empty / unknown.
bool from_string(const char *s, int *out);

// Validate a numeric form. Returns true iff in [kMin, kMax].
bool is_valid_int(int level);

}  // namespace log_level_check
