#pragma once

// Pure NMEA0183 sentence parser. No Arduino deps - compiles for both
// firmware and host so it can be unit-tested.
//
// Scope (first cut):
//   RMC, GGA, VTG  -> position, SOG, COG
//   VHW            -> heading, STW
//   HDT / HDG      -> heading true / heading magnetic
//   MWV            -> apparent/true wind angle + speed
//   DPT, DBT       -> depth below transducer
//   MTW            -> water temperature
//   XTE            -> cross-track error
//   BWC            -> bearing/distance to waypoint
//
// Sentences with bad checksum or unrecognized type return ok=false.

#include <stddef.h>
#include <stdint.h>

namespace nmea0183 {

enum class FieldKind : uint8_t {
    None = 0,
    LatDeg,          // signed degrees (N+, S-)
    LonDeg,          // signed degrees (E+, W-)
    SogKn,           // knots
    StwKn,           // knots
    CogTrueDeg,      // 0..360
    HeadingTrueDeg,  // 0..360
    HeadingMagDeg,   // 0..360
    AwaDeg,          // -180..180 (NMEA gives 0..360 relative)
    AwsKn,
    TwaDeg,
    TwsKn,
    DepthM,  // metres below transducer
    WaterTempC,
    XteNm,  // nautical miles, signed (+ = steer right)
    BtwTrueDeg,
    DtwNm,
};

struct FieldUpdate {
    FieldKind kind;
    double value;
};

struct ParseResult {
    bool ok;           // checksum valid AND sentence recognized
    char talker[3];    // null-terminated, e.g. "GP", "II"
    char sentence[4];  // null-terminated, e.g. "RMC"
    uint8_t count;
    FieldUpdate fields[8];
};

// Parse a single sentence. `line` may include or omit the leading `$`,
// and may include trailing CR/LF or not. Length is the byte count.
ParseResult parse_sentence(const char *line, size_t len);

// Streaming parser - feed raw socket bytes, get callbacks per complete
// sentence. Buffers up to 96 chars between sentences.
struct Stream {
    using Callback = void (*)(const ParseResult &r, void *user);
    Callback cb;
    void *user;
    char buf[96];
    size_t pos;
    bool inside;
};

void stream_init(Stream &s, Stream::Callback cb, void *user);
void stream_feed(Stream &s, const uint8_t *bytes, size_t n);

// Verify and strip an NMEA checksum. Returns the payload length (chars
// between '$' and '*') on success, or 0 on failure. Writes the
// detected checksum into *cksum_out when non-null.
size_t verify_checksum(const char *line, size_t len, uint8_t *cksum_out);

// Convert ddmm.mmmm and N/S or E/W into signed degrees. Empty/invalid
// strings yield NaN.
double parse_lat_lon(const char *value, char hemi);

}  // namespace nmea0183
