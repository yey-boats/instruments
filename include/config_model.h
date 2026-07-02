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

#include "value_format.h"

namespace config {

// Per-unit-class display formatting (decimals + optional k/M scaling). Rides
// the Ui domain for persistence/sync. Defaults: distance/depth scale large
// magnitudes (1234.5->"1.23k", 2841->"2.8k") so values fit the small tiles;
// speed/angle/temp/volts/percent keep their legacy fixed-decimal form. The
// painters (ui_layouts) and manager schema both consume this.
struct FormatConfig {
    vfmt::UnitFormat distance = {2, true};      // nm / m (XTE, DTW, log) — only k/M class
    vfmt::UnitFormat depth = {1, false};        // m below transducer/keel: bounded (<~200 m),
                                                // never k/M-scale (design review S1)
    vfmt::UnitFormat speed = {1, false};        // kn (SOG/STW/AWS/TWS/VMG)
    vfmt::UnitFormat angle = {0, false};        // deg (HDG/COG/BTW/CTS/AWA/TWA/rudder)
    vfmt::UnitFormat temperature = {1, false};  // C
    vfmt::UnitFormat voltage = {2, false};      // V
    vfmt::UnitFormat percent = {0, false};      // % (SOC, tanks)
};

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

// Theme ids are persisted to NVS as their u8 value — append only, never
// reorder. day/night/high-contrast mirror the MIDL catalog; red-night and
// classic are firmware-extra skins (not in the advertised manifest yet).
enum class Theme : uint8_t { Night = 0, Day = 1, HighContrast = 2, RedNight = 3, Classic = 4 };
enum class PosFormat : uint8_t { DDM = 0, DD = 1, DMS = 2 };

struct UiConfig {
    Theme theme = Theme::Night;
    // Default to max PWM (255). The Sunton 4848S040's transflective-ish
    // panel doesn't get readable below ~80% of full PWM under typical
    // indoor lighting, and there's no upside to factory-defaulting low -
    // the user can always dial it down from the Settings screen. The
    // segmented control in screen_settings.cpp offers {32,80,128,200,255}
    // so any of those choices remain valid post-load.
    uint8_t brightness = 255;
    PosFormat pos_format = PosFormat::DDM;
    char default_screen[16] = "dashboard";
    FormatConfig format;
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
