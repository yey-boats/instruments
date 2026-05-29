#pragma once

// Pure mapping from the strings the manager/plugin send in
// cfg["sources"]["priority"] / ["timeoutsMs"] to boat::SourceKind.
//
// The plugin contract names sources after their transport
// (`signalk` / `nmea0183Wifi` / `nmea2000` / `demo`) but the legacy
// source_name() output uses `nmea-wifi` for NmeaWifi. Accept both so
// any direction of drift between firmware and plugin doesn't break
// apply_config.

#include "boat_data.h"

namespace sources_check {

// Map a config string to a SourceKind. Returns SourceKind::None for
// unknown / null input. Case-sensitive.
boat::SourceKind from_string(const char *s);

}  // namespace sources_check
