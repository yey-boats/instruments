#pragma once

// Configuration domain model per docs/specs/08-configuration-storage-sync.md.
//
// One RAM-resident `RuntimeConfig` is the live source of truth. Render and
// alarm/refresh paths read from RAM only. Persistence is debounced and runs
// on a serialized storage worker (see config_store). External fetch/push is
// not implemented in this first cut.
//
// Domain coverage in this iteration:
//   - Ui (theme + brightness + pos_format + default_screen)
//   - Alarms (shallow depth threshold, low battery voltage)
//   - SignalK (host, port, token-ref)
//
// Identity / WiFi / Layout / Widgets / Hardware remain in their current
// modules until the migration moves them through here.

#include <stdint.h>
#include <stddef.h>

namespace config {

enum class Domain : uint8_t { Ui = 0, Alarms = 1, SignalK = 2, COUNT };

enum class Source : uint8_t {
    Default = 0,
    Storage = 1,
    Web = 2,
    Ble = 3,
    Serial = 4,
    External = 5,  // server/SignalK sync
};

enum class PersistPolicy : uint8_t {
    Debounced,
    ImmediateSafe,
    Manual,
    Never,
};

enum class Theme : uint8_t { Night = 0, Day = 1 };
enum class PosFormat : uint8_t { DDM = 0, DD = 1, DMS = 2 };

struct UiConfig {
    Theme theme = Theme::Night;
    uint8_t brightness = 200;
    PosFormat pos_format = PosFormat::DDM;
    char default_screen[16] = "dashboard";
};

struct AlarmConfig {
    double depth_alarm_m = 3.0;
    double battery_alarm_v = 11.5;
    bool audible = false;
};

struct SignalKConfig {
    char host[64] = "";
    uint16_t port = 3000;
    // Token is sensitive - we store it but never publish via /api/config.
    char token[80] = "";
};

struct DomainMeta {
    uint16_t schema = 1;
    Source source = Source::Default;
    uint32_t revision = 0;
    uint32_t updated_ms = 0;
    bool dirty = false;            // RAM differs from persisted checkpoint
    bool persist_pending = false;  // write queued/debounced
    bool persist_error = false;    // last write failed
    char last_error[64] = "";
};

struct RuntimeConfig {
    UiConfig ui;
    AlarmConfig alarms;
    SignalKConfig signalk;
    DomainMeta meta[(uint8_t)Domain::COUNT];
};

// Pure validators - host-testable. Return true if the candidate value is
// within bounds; out-of-range values get clamped, not rejected.
bool clamp_ui(UiConfig &c);
bool clamp_alarms(AlarmConfig &c);
bool clamp_signalk(SignalKConfig &c);

const char *theme_name(Theme t);
Theme parse_theme(const char *s, Theme fallback);
const char *pos_format_name(PosFormat f);
PosFormat parse_pos_format(const char *s, PosFormat fallback);

const char *domain_name(Domain d);
const char *source_name(Source s);

}  // namespace config
