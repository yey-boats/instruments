#pragma once

// Pure NMEA2000 PGN payload decoders + minimal fast-packet reassembly.
//
// No Arduino/TWAI dependencies (only <math.h>/<stdint.h>) so the decode math
// is host-testable from the Unity suites the same way nmea0183.h is; the
// device adapter (src/source_nmea2000.cpp) maps the decoded SI values onto
// boat::publish. Header-only inline so no build-filter change is needed.
//
// Field layouts follow the public canboat PGN database. All outputs are SI
// (rad, rad/s, m/s, Pa, K, Hz, m3/s); "not available" raw sentinels
// (0xFF/0x7FFF/...) decode to NaN.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace n2k {

inline uint16_t u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline int16_t s16le(const uint8_t *p) {
    return (int16_t)u16le(p);
}
inline uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int32_t s32le(const uint8_t *p) {
    return (int32_t)u32le(p);
}

// Raw -> SI with the canboat "not available" sentinels mapped to NaN.
inline double u16_res(const uint8_t *p, double res) {
    uint16_t v = u16le(p);
    return v == 0xFFFF ? NAN : v * res;
}
inline double s16_res(const uint8_t *p, double res) {
    int16_t v = s16le(p);
    return v == 0x7FFF ? NAN : v * res;
}
inline double s32_res(const uint8_t *p, double res) {
    int32_t v = s32le(p);
    return v == 0x7FFFFFFF ? NAN : v * res;
}

// ---------------------------------------------------------------------------
// PGN 127250 — Vessel Heading. BUG-2: the reference byte matters — heading may
// be TRUE (ref 0) or MAGNETIC (ref 1); collapsing both into heading_true
// conflates the two references.
struct Heading127250 {
    double heading_rad = NAN;    // per `reference`
    double deviation_rad = NAN;  // signed
    double variation_rad = NAN;  // signed, +E
    uint8_t reference = 0xFF;    // 0 = true, 1 = magnetic, else unknown
};
inline Heading127250 decode_127250(const uint8_t *d, uint8_t n) {
    Heading127250 r;
    if (n < 3) return r;
    r.heading_rad = u16_res(d + 1, 0.0001);
    if (n >= 5) r.deviation_rad = s16_res(d + 3, 0.0001);
    if (n >= 7) r.variation_rad = s16_res(d + 5, 0.0001);
    if (n >= 8) r.reference = d[7] & 0x03;
    return r;
}

// PGN 127251 — Rate of Turn. SID(0), rate s32 * 3.125e-8 rad/s. +ve = to stbd.
inline double decode_127251_rot_radps(const uint8_t *d, uint8_t n) {
    if (n < 5) return NAN;
    return s32_res(d + 1, 3.125e-8);
}

// PGN 127257 — Attitude. SID(0), yaw/pitch/roll s16 * 1e-4 rad.
struct Attitude127257 {
    double yaw_rad = NAN;
    double pitch_rad = NAN;  // +ve = bow up
    double roll_rad = NAN;   // +ve = heel to starboard
};
inline Attitude127257 decode_127257(const uint8_t *d, uint8_t n) {
    Attitude127257 r;
    if (n >= 3) r.yaw_rad = s16_res(d + 1, 0.0001);
    if (n >= 5) r.pitch_rad = s16_res(d + 3, 0.0001);
    if (n >= 7) r.roll_rad = s16_res(d + 5, 0.0001);
    return r;
}

// PGN 127245 — Rudder. instance(0), direction order(1), angle order s16(2-3),
// position s16(4-5), all angles * 1e-4 rad. Position is the measured angle.
struct Rudder127245 {
    uint8_t instance = 0xFF;
    double angle_order_rad = NAN;
    double position_rad = NAN;  // +ve = starboard helm
};
inline Rudder127245 decode_127245(const uint8_t *d, uint8_t n) {
    Rudder127245 r;
    if (n < 6) return r;
    r.instance = d[0];
    r.angle_order_rad = s16_res(d + 2, 0.0001);
    r.position_rad = s16_res(d + 4, 0.0001);
    return r;
}

// PGN 127488 — Engine Parameters Rapid. instance(0), speed u16 * 0.25 RPM
// (1-2), boost pressure u16 * 100 Pa (3-4), tilt/trim s8 % (5).
struct EngineRapid127488 {
    uint8_t instance = 0xFF;
    double rev_hz = NAN;  // SI revolutions (Hz); *60 for display RPM
    double boost_pressure_pa = NAN;
};
inline EngineRapid127488 decode_127488(const uint8_t *d, uint8_t n) {
    EngineRapid127488 r;
    if (n < 3) return r;
    r.instance = d[0];
    double rpm = u16_res(d + 1, 0.25);
    r.rev_hz = isnan(rpm) ? NAN : rpm / 60.0;
    if (n >= 5) r.boost_pressure_pa = u16_res(d + 3, 100.0);
    return r;
}

// PGN 127489 — Engine Parameters Dynamic (FAST PACKET, 26-byte payload).
// instance(0), oil pressure u16*100 Pa (1-2), oil temp u16*0.1 K (3-4),
// coolant temp u16*0.01 K (5-6), alternator s16*0.01 V (7-8), fuel rate
// s16*0.1 L/h (9-10), total engine hours u32 s (11-14), coolant pressure
// u16*100 Pa (15-16), fuel pressure u16*1000 Pa (17-18), ...
// NOTE: engine HOURS has no boat::Snapshot field yet — decoded here for
// completeness but not published (follow-up if an hours readout is wanted).
struct EngineDynamic127489 {
    uint8_t instance = 0xFF;
    double oil_pressure_pa = NAN;
    double oil_temp_k = NAN;
    double coolant_temp_k = NAN;
    double alternator_v = NAN;
    double fuel_rate_m3s = NAN;   // SI; raw is 0.1 L/h
    double engine_hours_s = NAN;  // no Snapshot field — see note above
};
inline EngineDynamic127489 decode_127489(const uint8_t *d, uint8_t n) {
    EngineDynamic127489 r;
    if (n < 3) return r;
    r.instance = d[0];
    r.oil_pressure_pa = u16_res(d + 1, 100.0);
    if (n >= 5) r.oil_temp_k = u16_res(d + 3, 0.1);
    if (n >= 7) r.coolant_temp_k = u16_res(d + 5, 0.01);
    if (n >= 9) r.alternator_v = s16_res(d + 7, 0.01);
    if (n >= 11) {
        double lph = s16_res(d + 9, 0.1);  // L/h
        r.fuel_rate_m3s = isnan(lph) ? NAN : lph * 1e-3 / 3600.0;
    }
    if (n >= 15) {
        uint32_t hours = u32le(d + 11);
        if (hours != 0xFFFFFFFF) r.engine_hours_s = (double)hours;
    }
    return r;
}

// ---------------------------------------------------------------------------
// PGN 130306 — Wind Data. BUG-3: map the reference byte properly instead of
// collapsing every non-apparent reference into TWA.
enum class WindTarget : uint8_t {
    Apparent,  // ref 2 -> AWA/AWS
    True,      // ref 0 (true, ground-referenced) / ref 3 (true, boat-referenced)
    Drop,      // ref 1 (magnetic ground wind direction — no field) + unknown refs
};
struct Wind130306 {
    double speed_mps = NAN;
    double angle_rad = NAN;  // raw [0, 2pi); caller wraps to signed if needed
    uint8_t reference = 0xFF;
    WindTarget target = WindTarget::Drop;
};
inline Wind130306 decode_130306(const uint8_t *d, uint8_t n) {
    Wind130306 r;
    if (n < 6) return r;
    r.speed_mps = u16_res(d + 1, 0.01);
    r.angle_rad = u16_res(d + 3, 0.0001);
    r.reference = d[5] & 0x07;
    switch (r.reference) {
    case 2:
        r.target = WindTarget::Apparent;
        break;
    case 0:  // true wind, ground referenced (theoretical, north up per canboat)
    case 3:  // true wind, boat referenced (relative to bow)
        r.target = WindTarget::True;
        break;
    default:
        // ref 1 = magnetic ground wind direction (no wind-direction field on
        // Snapshot), 4.. = reserved. Dropped WITH a counted stat by the caller
        // — never silently folded into TWA.
        r.target = WindTarget::Drop;
        break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Minimal ISO 11783 fast-packet reassembly (single concurrent stream — one
// state per PGN of interest). Frame 0 carries [seq|0, total_len, 6 data
// bytes]; frames 1..N carry [seq|frame, 7 data bytes]. Returns true when the
// full payload is available in fp.buf[0..fp.expect).
// buf is sized for the largest PGN we reassemble (129794 AIS Class A static
// = 75 bytes; was 64 when 127489's 26 bytes was the only fast-packet user).
struct FastPacket {
    uint8_t buf[96];
    uint8_t len = 0;         // bytes assembled so far
    uint8_t expect = 0;      // total payload length from frame 0
    uint8_t seq = 0xFF;      // current sequence id; 0xFF = idle
    uint8_t next_frame = 0;  // next expected frame counter
};
inline bool fastpacket_feed(FastPacket &fp, const uint8_t *d, uint8_t n) {
    if (!d || n < 2) return false;
    uint8_t seq = (uint8_t)(d[0] >> 5);
    uint8_t frame = (uint8_t)(d[0] & 0x1F);
    if (frame == 0) {
        fp.seq = seq;
        fp.expect = d[1];
        fp.len = 0;
        fp.next_frame = 1;
        if (fp.expect == 0 || fp.expect > sizeof(fp.buf)) {
            fp.seq = 0xFF;  // bogus length — drop the stream
            return false;
        }
        uint8_t take = (uint8_t)(n - 2);
        if (take > 6) take = 6;
        memcpy(fp.buf, d + 2, take);
        fp.len = take;
        return fp.len >= fp.expect;
    }
    // Continuation: must match the open stream and arrive in order.
    if (fp.seq != seq || frame != fp.next_frame) {
        fp.seq = 0xFF;  // out-of-order/foreign frame — reset, wait for frame 0
        return false;
    }
    uint8_t take = (uint8_t)(n - 1);
    if ((uint16_t)fp.len + take > sizeof(fp.buf)) take = (uint8_t)(sizeof(fp.buf) - fp.len);
    memcpy(fp.buf + fp.len, d + 1, take);
    fp.len = (uint8_t)(fp.len + take);
    fp.next_frame++;
    return fp.len >= fp.expect;
}

// ---------------------------------------------------------------------------
// Multi-stream fast-packet reassembly. AIS position bursts (129038/129039
// from several targets, plus 127489 engine frames) interleave on the bus, and
// distinct (PGN, source address) streams must not reset each other — the
// single-stream FastPacket above corrupts under that interleave. Small
// fixed pool: streams are keyed by (pgn, src); frame 0 claims a free slot or
// steals the STALEST one (oldest last_ms) when all 4 are busy (a stolen
// stream simply re-syncs on its next frame 0).
struct FastPacketPool {
    static constexpr int SLOTS = 4;
    struct Slot {
        uint32_t pgn = 0;
        uint8_t src = 0xFF;
        bool active = false;
        uint32_t last_ms = 0;
        FastPacket fp;
    };
    Slot slots[SLOTS];
};

// Feed one CAN frame belonging to fast-packet stream (pgn, src). Returns the
// completed FastPacket (payload in ->buf[0..->expect)) or nullptr while the
// stream is still assembling / on error. The returned stream's slot is
// released before returning, so the pointer is only valid until the next
// feed on the same pool (single-task use, matching the device worker).
inline FastPacket *fastpacket_pool_feed(FastPacketPool &pool, uint32_t pgn, uint8_t src,
                                        const uint8_t *d, uint8_t n, uint32_t now_ms) {
    if (!d || n < 2) return nullptr;
    uint8_t frame = (uint8_t)(d[0] & 0x1F);
    // Existing stream for this (pgn, src)?
    FastPacketPool::Slot *slot = nullptr;
    for (auto &s : pool.slots) {
        if (s.active && s.pgn == pgn && s.src == src) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        if (frame != 0) return nullptr;  // mid-stream frame for a stream we never opened
        // Claim a free slot, else steal the stalest.
        for (auto &s : pool.slots) {
            if (!s.active) {
                slot = &s;
                break;
            }
        }
        if (!slot) {
            slot = &pool.slots[0];
            for (auto &s : pool.slots)
                if ((int32_t)(s.last_ms - slot->last_ms) < 0) slot = &s;
        }
        slot->pgn = pgn;
        slot->src = src;
        slot->active = true;
        slot->fp.seq = 0xFF;
    }
    slot->last_ms = now_ms;
    bool done = fastpacket_feed(slot->fp, d, n);
    if (done) {
        slot->active = false;  // released; payload stays readable until reuse
        return &slot->fp;
    }
    if (slot->fp.seq == 0xFF) slot->active = false;  // feed reset (bogus/ooo) — free the slot
    return nullptr;
}

// ---------------------------------------------------------------------------
// AIS PGNs (fast packet). Layouts per canboat.
//
// PGN 129038 — Class A Position Report — and PGN 129039 — Class B Position
// Report — share the fields we consume:
//   byte 0      message id (6 bits) + repeat (2 bits)
//   bytes 1-4   user id (MMSI), u32
//   bytes 5-8   longitude, s32 * 1e-7 deg
//   bytes 9-12  latitude,  s32 * 1e-7 deg
//   byte 13     accuracy/RAIM/time stamp
//   bytes 14-15 COG, u16 * 1e-4 rad
//   bytes 16-17 SOG, u16 * 0.01 m/s
//   bytes 18-20 communication state + transceiver info
//   bytes 21-22 true heading, u16 * 1e-4 rad (often unavailable on Class B)
struct AisPosition {
    uint32_t mmsi = 0;
    double lat_deg = NAN;
    double lon_deg = NAN;
    double sog_mps = NAN;
    double cog_rad = NAN;
    double heading_rad = NAN;
};
inline AisPosition decode_ais_position(const uint8_t *d, uint8_t n) {
    AisPosition r;
    if (!d || n < 13) return r;
    r.mmsi = u32le(d + 1);
    r.lon_deg = s32_res(d + 5, 1e-7);
    r.lat_deg = s32_res(d + 9, 1e-7);
    if (n >= 16) r.cog_rad = u16_res(d + 14, 0.0001);
    if (n >= 18) r.sog_mps = u16_res(d + 16, 0.01);
    if (n >= 23) r.heading_rad = u16_res(d + 21, 0.0001);
    return r;
}
inline AisPosition decode_129038(const uint8_t *d, uint8_t n) {
    return decode_ais_position(d, n);
}
inline AisPosition decode_129039(const uint8_t *d, uint8_t n) {
    return decode_ais_position(d, n);
}

// PGN 129794 — AIS Class A Static and Voyage Related Data (75-byte payload).
//   byte 0      message id + repeat
//   bytes 1-4   user id (MMSI), u32
//   bytes 5-8   IMO number, u32
//   bytes 9-15  callsign, 7 ASCII
//   bytes 16-35 name, 20 ASCII ('@'/space padded per ITU-R M.1371)
struct AisStatic129794 {
    uint32_t mmsi = 0;
    char name[21] = {0};  // trimmed, NUL-terminated
};
inline AisStatic129794 decode_129794(const uint8_t *d, uint8_t n) {
    AisStatic129794 r;
    if (!d || n < 36) return r;
    r.mmsi = u32le(d + 1);
    memcpy(r.name, d + 16, 20);
    r.name[20] = 0;
    // Trim AIS padding: trailing '@', spaces, and 0xFF "no data" bytes.
    for (int i = 19; i >= 0; --i) {
        char c = r.name[i];
        if (c == '@' || c == ' ' || (uint8_t)c == 0xFF || c == 0)
            r.name[i] = 0;
        else
            break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// PGN 65288 — Raymarine Seatalk Alarm (single frame, proprietary).
//   bytes 0-1  manufacturer code (1851) + industry
//   byte 2     SID
//   byte 3     alarm status (0 = condition not met, 1 = met not silenced,
//              2 = met and silenced)
//   byte 4     alarm id
//   byte 5     alarm group (0 instrument, 1 autopilot, 2 radar, ...)
//   bytes 6-7  alarm priority
struct SeatalkAlarm65288 {
    uint8_t status = 0xFF;
    uint8_t alarm_id = 0xFF;
    uint8_t group = 0xFF;
    bool valid = false;
};
inline SeatalkAlarm65288 decode_65288(const uint8_t *d, uint8_t n) {
    SeatalkAlarm65288 r;
    if (!d || n < 6) return r;
    r.status = d[3];
    r.alarm_id = d[4];
    r.group = d[5];
    r.valid = true;
    return r;
}

}  // namespace n2k
