#include "sources_check.h"

#include <string.h>

namespace sources_check {

boat::SourceKind from_string(const char *s) {
    if (!s || !*s) return boat::SourceKind::None;

    if (strcmp(s, "signalk") == 0)       return boat::SourceKind::SignalK;
    if (strcmp(s, "demo") == 0)          return boat::SourceKind::Demo;
    if (strcmp(s, "none") == 0)          return boat::SourceKind::None;

    // NmeaWifi accepts both source_name() output and plugin canonical.
    if (strcmp(s, "nmea-wifi") == 0)     return boat::SourceKind::NmeaWifi;
    if (strcmp(s, "nmeaWifi") == 0)      return boat::SourceKind::NmeaWifi;
    if (strcmp(s, "nmea0183Wifi") == 0)  return boat::SourceKind::NmeaWifi;

    if (strcmp(s, "nmea2000") == 0)      return boat::SourceKind::Nmea2000;

    return boat::SourceKind::None;
}

}  // namespace sources_check
